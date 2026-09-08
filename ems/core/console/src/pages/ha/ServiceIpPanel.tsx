import { Alert } from '@core/components/ui/alert'
import { Lock, Pencil, Trash2 } from 'lucide-react'
import { useState } from 'react'
import type { AgentRoute } from '../../api/deployment'
import type { NetIface, ServiceIpRow, IpSlot } from './types'
import { ImeSafeInput } from './ImeSafeInput'
import { Badge } from '../../components/ui/badge'
import { Button } from '../../components/ui/button'
import { DataTable, Th, Td, orDash } from '../../components/custom/data-table'

// ServiceIpPanel — 인터페이스별 cims-managed IP 추가/삭제 + specific route 관리.
// 모델: 각 IP 가 row (iface, ip 단위). agent 가 보고한 interfaces.managed=true 인 IP 만
// [삭제] 허용 (외부 IP 는 readonly). [+IP 추가] / [+라우팅 추가] 로 명시적 op 발사.
// route 는 kernel 외 default GW + specific subnet 모두 변경 가능.
export function ServiceIpPanel({ title, section = 'both', interfaces, storedRows, storedRoutes, slots, applying,
                                 onApply, onUpdateSlot, vipIps }: {
  title: string
  /** 시안은 `IP / Routing` 과 `라우팅` 을 **별도 Level 2 섹션**으로 둔다(Figma S1 49:1369).
      호출자가 두 번 렌더해 각각 제 섹션에 넣는다. 다른 화면에서는 'both' 로 통째로 쓴다. */
  section?: 'ip' | 'routes' | 'both'
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
      `route 수정 ${orig.dst} → via ${routeEditVia} dev ${routeEditDev}`,
    )
    setRouteEditKey(null)
  }

  return (
    // 상위 SubSection 이 제목·힌트·들여쓰기를 그린다 — 회색 패널을 또 두르지 않는다.
    <div>
      {title && (
        <div className="mb-2 text-sm font-semibold text-muted-foreground">
          {title}
          <span className="ml-2 text-xs font-normal">
            (cims-managed 만 변경 가능 — 외부 IP / mgmt NIC 은 보호)
          </span>
        </div>
      )}

      {section !== 'routes' && (<>
      {/* 시안은 이 안내를 표 **위** SectionMessage 로 둔다 (Figma S1 49:1519) */}
      <Alert variant="info" className="mb-2.5">
        {slots.length > 0
          ? <>참고 — 설치된 패키지의 권장 용도: <code className="font-mono">{slotHints}</code> (자유 입력 가능)</>
          : <>
              <div className="font-medium">인프라 단계에서는 NIC 이름이 곧 용도 라벨로 사용됩니다</div>
              <div className="mt-0.5 text-xs opacity-90">
                슬롯을 비워두면 해당 인터페이스는 라벨 없이 등록됩니다.
              </div>
            </>}
      </Alert>
      {/* 컬럼 폭은 Figma S1 실측 */}
      <DataTable>
        <thead>
          <tr>
            <Th width={140}>인터페이스</Th>
            <Th width={190}>IP / mask</Th>
            <Th width={170}>용도 (slot)</Th>
            <Th width={110}>소유</Th>
            <Th width={160} align="right">액션</Th>
          </tr>
        </thead>
        <tbody>
          {ifaceOrder.length === 0 && (
            <tr><Td colSpan={5} className="text-muted-foreground">인터페이스 없음 — agent 보고 대기</Td></tr>
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
                    <tr key={`${iface}-${ni.ip}-${ipIdx}`}>
                      <Td mono>
                        {ipIdx === 0 && iface}
                        {ipIdx === 0 && isMgmt && (
                          <span className="ml-1.5 inline-flex items-center gap-1 text-xs text-muted-foreground"
                                title="agent ↔ CSC 통신 NIC — 변경 시 단절 위험으로 잠금">
                            <Lock size={11} /> mgmt
                          </span>
                        )}
                      </Td>
                      <Td mono>{ni.ip}/{ni.mask}</Td>
                      <Td>
                        {/* 용도(slot) — NIC 의 단일 분류 키. VIP→NIC 매핑도 이 값으로 결정.
                            VIP 는 HA 그룹 바인딩에서 결정 → 읽기전용. mgmt 는 IP 값은 잠금이나
                            용도(slot)는 입력 가능 (mgmt NIC 도 운용자가 분류 라벨 지정). */}
                        {isVip ? (
                          <span className="text-muted-foreground">{slot || '—'}</span>
                        ) : (
                          <ImeSafeInput value={slot} placeholder="(용도)" className="form-input"
                                        onCommit={(v) => {
                                          if (v !== slot) onUpdateSlot(iface, ni.ip, ni.mask, v)
                                        }} />
                        )}
                      </Td>
                      <Td>
                        {/* 소유 배지 톤은 시안 실측: mgmt=neutral · 외부=info · VIP=brand.
                            cims-managed 는 우리 것이 정상 동작이라 success. */}
                        {isMgmtIp ? <Badge variant="neutralSoft">mgmt</Badge>
                          : isVip ? <Badge variant="brandSoft">VIP</Badge>
                          : managed ? <Badge variant="successSoft">cims</Badge>
                          : <Badge variant="infoSoft">외부</Badge>}
                      </Td>
                      <Td align="right">
                        {managed && !isMgmtIp && !isVip
                          ? <Button variant="destructive" disabled={applying}
                                    onClick={() => deleteIp(iface, ni.ip, ni.mask)}>
                              <Trash2 /> 삭제
                            </Button>
                          : <span className="text-muted-foreground">—</span>}
                      </Td>
                    </tr>
                  )
                })
              : [(
                  <tr key={`${iface}-empty`}>
                    <Td mono>{iface}</Td>
                    <Td colSpan={4} className="text-muted-foreground">IP 미할당</Td>
                  </tr>
                )]
            return ifaceRows
          })}
          {addOpen && (
            <tr className="bg-warning-soft">
              <Td>
                <select value={addIface} onChange={e => setAddIface(e.target.value)} className="form-input">
                  {addableIfaces.length === 0 && <option value="">(없음)</option>}
                  {addableIfaces.map(name => <option key={name} value={name}>{name}</option>)}
                </select>
              </Td>
              <Td>
                <div className="flex items-center gap-1.5">
                  <span className="inline-block w-[120px]">
                    <input value={addIp} placeholder="10.0.3.45" className="form-input font-mono"
                           onChange={e => setAddIp(e.target.value)} />
                  </span>
                  <span className="text-muted-foreground">/</span>
                  <span className="inline-block w-[56px]">
                    <input type="number" value={addMask} className="form-input font-mono"
                           onChange={e => setAddMask(parseInt(e.target.value) || 24)} />
                  </span>
                </div>
              </Td>
              <Td>
                <ImeSafeInput value={addSlot} onCommit={setAddSlot} placeholder="(용도)"
                              className="form-input" />
              </Td>
              <Td colSpan={2} align="right">
                <div className="flex items-center justify-end gap-1.5">
                  <Button variant="default" onClick={commitAdd}
                          disabled={!addIface || !addIp || !addMask || applying}>추가</Button>
                  <Button variant="ghost" onClick={cancelAdd}>취소</Button>
                </div>
              </Td>
            </tr>
          )}
        </tbody>
      </DataTable>
      {addableIfaces.length > 0 && (
        <Button variant="outline" className="mt-2.5" onClick={beginAdd} disabled={applying || addOpen}>
          + IP 추가
        </Button>
      )}
      </>)}

      {section !== 'ip' && (<>
      {section === 'both' && (
        <div className="mb-2 mt-4 text-sm font-semibold text-muted-foreground">
          라우팅{' '}
          <span className="font-normal">(subnet 자동(kernel) 외 모두 변경 가능 — default gateway 포함)</span>
        </div>
      )}
      <DataTable>
        <thead>
          <tr>
            <Th width={200}>dest CIDR</Th>
            <Th width={180}>gateway</Th>
            <Th width={110}>dev</Th>
            <Th width={120}>소유</Th>
            <Th width={160} align="right">액션</Th>
          </tr>
        </thead>
        <tbody>
          {storedRoutes.length === 0 && !routeAddOpen && (
            <tr><Td colSpan={5} className="text-muted-foreground">라우팅 없음</Td></tr>
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
            // kernel 은 배지가 아니라 자물쇠 + 흐린 글자다 (시안 실측) — 잠긴 사실이지 분류가 아니다.
            const ownerChip = kernelAuto
              ? <span className="inline-flex items-center gap-1 text-muted-foreground"><Lock size={11} /> kernel</span>
              : isDefault ? <Badge variant="brandSoft">default</Badge>
              : managed   ? <Badge variant="successSoft">cims</Badge>
              :             <Badge variant="infoSoft">외부</Badge>
            const canEdit = !kernelAuto
            const isEditing = routeEditKey === r.dst
            if (isEditing) {
              return (
                <tr key={rowKey} className="bg-warning-soft">
                  <Td mono>{r.dst}</Td>
                  <Td>
                    <span className="inline-block w-[140px]">
                      <input value={routeEditVia} className="form-input font-mono"
                             onChange={e => setRouteEditVia(e.target.value)} />
                    </span>
                  </Td>
                  <Td>
                    <select value={routeEditDev} onChange={e => setRouteEditDev(e.target.value)}
                            className="form-input">
                      {routableIfaces.map(d => <option key={d} value={d}>{d}</option>)}
                    </select>
                  </Td>
                  <Td>{ownerChip}</Td>
                  <Td align="right">
                    <div className="flex items-center justify-end gap-1.5">
                      <Button variant="default" onClick={() => commitEditRoute(r)}
                              disabled={!routeEditVia || !routeEditDev || applying ||
                                        (routeEditVia === r.via && routeEditDev === r.dev)}>저장</Button>
                      <Button variant="ghost" onClick={cancelEditRoute}>취소</Button>
                    </div>
                  </Td>
                </tr>
              )
            }
            return (
              <tr key={rowKey}>
                <Td mono>{r.dst}</Td>
                <Td mono>{orDash(r.via)}</Td>
                <Td mono>{orDash(r.dev)}</Td>
                <Td>{ownerChip}</Td>
                <Td align="right">
                  {canEdit
                    ? <div className="flex items-center justify-end gap-1.5">
                        <Button variant="ghost" onClick={() => beginEditRoute(r)} disabled={applying}>
                          <Pencil /> 수정
                        </Button>
                        <Button variant="destructive" onClick={() => deleteRoute(r)} disabled={applying}>
                          <Trash2 /> 삭제
                        </Button>
                      </div>
                    : <span className="text-muted-foreground">—</span>}
                </Td>
              </tr>
            )
          })}
          {routeAddOpen && (
            <tr className="bg-warning-soft">
              <Td>
                <input value={routeDst} placeholder="192.168.100.0/24" className="form-input font-mono"
                       onChange={e => setRouteDst(e.target.value)} />
              </Td>
              <Td>
                <input value={routeVia} placeholder="10.0.3.1" className="form-input font-mono"
                       onChange={e => setRouteVia(e.target.value)} />
              </Td>
              <Td>
                <select value={routeDev} onChange={e => setRouteDev(e.target.value)} className="form-input">
                  {routableIfaces.map(d => <option key={d} value={d}>{d}</option>)}
                  {routableIfaces.length === 0 && <option value="">(없음)</option>}
                </select>
              </Td>
              <Td colSpan={2} align="right">
                <div className="flex items-center justify-end gap-1.5">
                  <Button variant="default" onClick={commitAddRoute}
                          disabled={!routeDst || !routeVia || !routeDev || applying}>추가</Button>
                  <Button variant="ghost" onClick={cancelAddRoute}>취소</Button>
                </div>
              </Td>
            </tr>
          )}
        </tbody>
      </DataTable>
      <Button variant="outline" className="mt-2.5" onClick={beginAddRoute}
              disabled={applying || routableIfaces.length === 0 || routeAddOpen}
              title={routableIfaces.length === 0 ? 'mgmt 외 NIC 없음' : 'route 추가 (default GW 포함)'}>
        + 라우팅 추가
      </Button>
      {routableIfaces.length === 0 && (
        <div className="mt-1 text-xs text-muted-foreground">
          [+ 라우팅 추가] 는 mgmt 외 NIC 이 있어야 열립니다.
        </div>
      )}
      </>)}
    </div>
  )
}
