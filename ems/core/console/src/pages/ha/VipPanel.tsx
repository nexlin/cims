// ──────────────────────────────────────────────────────────────
//  VipPanel — VIP slot 단위 row (HA active_standby / all_active 그룹)
//  용도(slot) 선택 시 멤버별 iface 자동 매핑 — 옵션은 각 서버 ServiceIp 의 용도 라벨에서 옴.
// ──────────────────────────────────────────────────────────────

import { splitPrefixHost } from './helpers'
import { btnSmall, btnAdd } from './styles'
import type { ServiceRow, VipBinding, BindingStatus } from './types'

export function VipPanel({ title, svc, vrid, onChange, onApply }: {
  title: string
  svc: ServiceRow
  vrid?: number | null
  onChange: (bindings: VipBinding[]) => void
  onApply?: () => void
}) {
  const bindings = svc.vipBindings
  const servers = svc.servers

  // 용도 dropdown 옵션 = 각 서버 ServiceIp 에서 명시 입력한 용도(slot) 만.
  // 값에 iface + ip + mask 보관 → subnet 정합 검증 + VIP prefix 자동 결정에 사용.
  const slotMap = new Map<string, Map<number, { iface: string; ip: string; mask: number }>>()
  for (const srv of servers) {
    for (const r of srv.serviceIpRows) {
      if (!r.slot) continue
      if (!slotMap.has(r.slot)) slotMap.set(r.slot, new Map())
      slotMap.get(r.slot)!.set(srv.id, { iface: r.iface, ip: r.ip, mask: r.mask })
    }
  }
  const availableSlots = Array.from(slotMap.keys()).sort()

  // slot 별 subnet 정합 정보: 모든 멤버의 (prefix, mask) 일치 시 prefix 반환, 아니면 conflict.
  const slotSubnetInfo = (slot: string): {
    prefix: string | null; mask: number; conflict: boolean; conflictDetail: string
  } => {
    const m = slotMap.get(slot)
    if (!m || m.size === 0) return { prefix: null, mask: 24, conflict: false, conflictDetail: '' }
    const entries = Array.from(m.values())
    const first = splitPrefixHost(entries[0].ip, entries[0].mask)
    if (!first) return { prefix: null, mask: entries[0].mask, conflict: true,
                          conflictDetail: `비표준 mask=${entries[0].mask}` }
    for (const e of entries.slice(1)) {
      const p = splitPrefixHost(e.ip, e.mask)
      if (!p || p.prefix !== first.prefix || e.mask !== entries[0].mask) {
        return { prefix: first.prefix, mask: entries[0].mask, conflict: true,
                 conflictDetail: `${entries[0].ip}/${entries[0].mask} ≠ ${e.ip}/${e.mask}` }
      }
    }
    return { prefix: first.prefix, mask: entries[0].mask, conflict: false, conflictDetail: '' }
  }

  const autoMapMemberIfaces = (slot: string): { [id: number]: string } => {
    const result: { [id: number]: string } = {}
    const ifaceMap = slotMap.get(slot)
    if (ifaceMap) for (const [sid, info] of ifaceMap) result[sid] = info.iface
    return result
  }

  const addRow = () => {
    const newId = Math.max(0, ...bindings.map(b => b.bid)) + 1
    onChange([...bindings, {
      bid: newId, slot: '', ip: '', mask: 24, status: 'unknown', memberIfaces: {},
    }])
  }
  const updateRow = (bid: number, patch: Partial<VipBinding>) =>
    onChange(bindings.map(b => b.bid === bid ? { ...b, ...patch } : b))
  const removeRow = (bid: number) => onChange(bindings.filter(b => b.bid !== bid))

  const onSlotChange = (bid: number, newSlot: string) => {
    updateRow(bid, {
      slot: newSlot,
      memberIfaces: newSlot ? autoMapMemberIfaces(newSlot) : {},
      status: 'unknown',
      dirty: true,
    })
  }

  const applyRow = (bid: number) => {
    updateRow(bid, { status: 'unknown', dirty: false })
    onApply?.()    // ha-groups/{id}/apply 호출 → update_ha job 큐잉 → keepalived reload
  }

  // 멤버별 VIP 보유 여부: memberIfaces[serverId] 에 매핑된 iface 에 b.ip 가 실제 존재?
  const memberHasVip = (b: VipBinding, serverId: number): boolean => {
    if (!b.ip) return false
    const memberIface = b.memberIfaces?.[serverId]
    if (!memberIface) return false
    const srv = servers.find(s => s.id === serverId)
    if (!srv) return false
    return srv.interfaces.some(x => x.name === memberIface && x.ip === b.ip)
  }
  const bindingStatus = (b: VipBinding): BindingStatus => {
    // 사용자가 ip/slot 을 편집한 직후엔 적용 전이므로 NIC 매칭 의미 없음 → 'unknown'
    if (b.dirty) return 'unknown'
    if (!b.ip) return 'unknown'
    return servers.some(s => memberHasVip(b, s.id)) ? 'up' : 'down'
  }

  return (
    <div style={{
      borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px',
      background: 'var(--bg-soft)',
    }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)', marginBottom: 10 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)', fontWeight: 'normal' }}>
          (용도 선택 시 멤버별 iface 자동 매핑 — 옵션은 각 서버 ServiceIp 의 용도 라벨에서 옴. 수동 override 가능)
        </span>
      </div>

      {availableSlots.length === 0 && (
        <div style={{ padding: 10, background: 'var(--warn-soft)', border: '1px solid #f0c75e',
                      borderRadius: 4, fontSize: 12, color: '#876200', marginBottom: 8 }}>
          ⚠ 멤버 서버의 ServiceIp 에 "용도" 라벨이 입력된 항목이 없습니다.
          먼저 각 서버 인터페이스에 용도를 입력해야 VIP 의 용도 select 에 옵션이 표시됩니다.
        </div>
      )}

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 40 }}>#</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>용도</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 170 }}>VIP / mask</th>
            {servers.map(s => (
              <th key={s.id} style={{ padding: '4px 8px', textAlign: 'left' }}>
                {s.name} {s.role && <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>({s.role})</span>}
              </th>
            ))}
            {vrid != null && <th style={{ padding: '4px 8px', textAlign: 'left', width: 60 }}>VRID</th>}
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 130 }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {bindings.length === 0 && (
            <tr>
              <td colSpan={4 + servers.length + (vrid != null ? 1 : 0) + 1} style={{ padding: '8px', color: 'var(--text-muted)' }}>
                (VIP 없음 — 아래 [＋ VIP 추가])
              </td>
            </tr>
          )}
          {bindings.map((b, i) => {
            const usedSlots = new Set(bindings.map(x => x.slot).filter(Boolean))
            return (
              <tr key={b.bid}>
                <td style={{ padding: '4px 8px', color: 'var(--text-muted)' }}>{i + 1}</td>
                <td style={{ padding: '4px 8px' }}>
                  <select value={b.slot} onChange={e => onSlotChange(b.bid, e.target.value)}
                          style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                   color: b.slot ? '#333' : '#c00' }}>
                    <option value="">(용도 선택)</option>
                    {availableSlots.map(name => {
                      const mappedCount = slotMap.get(name)!.size
                      const complete = mappedCount === servers.length
                      const used = usedSlots.has(name) && b.slot !== name
                      const subnet = complete ? slotSubnetInfo(name) : null
                      const conflict = !!(subnet && subnet.conflict)
                      const disabled = !complete || used || conflict
                      const label = !complete
                        ? `${name} (${mappedCount}/${servers.length} 입력됨 — 모든 멤버 필요)`
                        : conflict ? `${name} (IP/mask 불일치: ${subnet!.conflictDetail})`
                        : used ? `${name} (사용중)`
                        : name
                      return (
                        <option key={name} value={name} disabled={disabled}>
                          {label}
                        </option>
                      )
                    })}
                  </select>
                </td>
                <td style={{ padding: '4px 8px' }}>
                  {(() => {
                    const subnet = b.slot ? slotSubnetInfo(b.slot) : null
                    const hasPrefix = !!(subnet && !subnet.conflict && subnet.prefix)
                    const split = hasPrefix && b.ip ? splitPrefixHost(b.ip, subnet!.mask) : null
                    if (hasPrefix) {
                      return (
                        <span style={{ display: 'inline-flex', gap: 2, alignItems: 'center', fontSize: 12 }}>
                          <span style={{ color: 'var(--text-muted)' }}>{subnet!.prefix}</span>
                          <input value={split?.host ?? ''}
                                 onChange={e => {
                                   const host = e.target.value.trim()
                                   updateRow(b.bid, {
                                     ip: host ? `${subnet!.prefix}${host}` : '',
                                     mask: subnet!.mask,
                                     status: 'unknown',
                                     dirty: true,
                                   })
                                 }}
                                 placeholder="host"
                                 style={{ width: subnet!.mask >= 24 ? 50 : 110, padding: '2px 6px', fontSize: 12,
                                          border: '1px solid var(--border)', borderRadius: 3 }} />
                          <span style={{ color: 'var(--text-muted)' }}>/{subnet!.mask}</span>
                        </span>
                      )
                    }
                    return (
                      <span style={{ display: 'inline-flex', gap: 2, alignItems: 'center' }}>
                        <input value={b.ip}
                               onChange={e => updateRow(b.bid, { ip: e.target.value, status: 'unknown', dirty: true })}
                               placeholder="(VIP)" disabled={!b.slot}
                               style={{ width: 110, padding: '2px 6px', fontSize: 12,
                                        border: '1px solid var(--border)', borderRadius: 3 }} />
                        <span>/</span>
                        <input type="number" value={b.mask ?? 24}
                               onChange={e => updateRow(b.bid, { mask: parseInt(e.target.value) || 24, dirty: true })}
                               disabled={!b.slot}
                               style={{ width: 40, padding: '2px 6px', fontSize: 12,
                                        border: '1px solid var(--border)', borderRadius: 3 }} />
                      </span>
                    )
                  })()}
                </td>
                {servers.map(s => {
                  const info = slotMap.get(b.slot)?.get(s.id)
                  const owns = memberHasVip(b, s.id)
                  return (
                    <td key={s.id} style={{ padding: '4px 8px', fontSize: 12 }}>
                      {!b.slot ? (
                        <span style={{ color: 'var(--text-muted)' }}>—</span>
                      ) : info ? (
                        <span>
                          {owns && <span title="이 멤버가 VIP 보유 (MASTER)"
                                          style={{ color: '#27ae60', marginRight: 4 }}>●</span>}
                          <b style={{ fontFamily: 'monospace' }}>{info.iface}</b>
                          <span style={{ marginLeft: 4, color: 'var(--text-muted)' }}>({info.ip}/{info.mask})</span>
                        </span>
                      ) : (
                        <span style={{ color: '#c0392b' }}>⚠ "{b.slot}" 매핑 없음</span>
                      )}
                    </td>
                  )
                })}
                {vrid != null && (
                  <td style={{ padding: '4px 8px', color: 'var(--text-muted)' }}>{vrid}</td>
                )}
                <td style={{ padding: '4px 8px' }}><StatusBadge status={bindingStatus(b)} /></td>
                <td style={{ padding: '4px 8px' }}>
                  <button onClick={() => applyRow(b.bid)} style={btnSmall()} title="VIP 적용 + up 확인">적용</button>
                  <button onClick={() => removeRow(b.bid)} style={btnSmall()} title="row 제거">삭제</button>
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>

      <div style={{ marginTop: 8 }}>
        <button onClick={addRow} style={btnAdd(true)}
                disabled={availableSlots.length === 0}>
          ＋ VIP 추가 {availableSlots.length === 0 && '(서버 용도 선설정 필요)'}
        </button>
      </div>
    </div>
  )
}

export function StatusBadge({ status }: { status?: BindingStatus }) {
  const s = status ?? 'unknown'
  const map: Record<BindingStatus, { icon: string; color: string; label: string }> = {
    up:       { icon: '●', color: '#27ae60', label: 'up' },
    down:     { icon: '◐', color: '#c0392b', label: 'down' },
    unknown:  { icon: '○', color: 'var(--text-muted)',    label: '미확인' },
    applying: { icon: '⏳', color: '#f39c12', label: '적용 중' },
    fail:     { icon: '✕', color: '#c0392b', label: '실패' },
    idle:     { icon: '—', color: 'var(--text-muted)',    label: '미할당' },
  }
  const m = map[s]
  return (
    <span style={{ fontSize: 12, color: m.color, fontWeight: 'bold' }}>
      {m.icon} {m.label}
    </span>
  )
}
