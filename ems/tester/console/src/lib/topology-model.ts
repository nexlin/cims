// 토폴로지 캔버스 모델 — 검증(컨트롤러 Topology 모델과 같은 규칙 + 편집 힌트)·파생 조회·팔레트 생성·이름 바꾸기·프리셋·자동 배치.
// 레코드 정본은 컨트롤러(`tester_models.Topology`); 여기서는 저장 전에 같은 오류를 미리 보이고 캔버스에 카드로 붙일 focus 를 만든다.
import type { TopologyDoc, TopoNode, PoolDoc, UePoolDoc, PeerPoolDoc, TopoLayout, NodeRole, Transport, SipListener } from '@tester/api/tester'

export type Level = 'err' | 'warn' | 'info'
export interface Focus { kind: 'host' | 'worker' | 'node' | 'pool'; id: string }
export interface Issue { level: Level; who: string; msg: string; focus: Focus | null }

export const ID_RE = /^[a-z][a-z0-9_]*$/
export const ROLE: Record<NodeRole, { label: string; fns: string[] }> = {
  sip: { label: 'SIP 서버', fns: ['CSP', 'P-CSCF', 'S-CSCF', 'I-CSCF', 'IBCF', 'SBC', 'PBX'] },
  tas: { label: 'TAS', fns: ['TAS', 'PTT-AS', 'MMTel AS'] },
  media: { label: '미디어 서버', fns: ['CMP', 'MRF', 'TrGW', 'MGW', 'PBX 미디어'] },
  subscriber: { label: '가입자 서버', fns: ['CSC', 'HSS', 'UDM'] },
  oam: { label: 'OAM', fns: ['OAM'] },
  db: { label: '가입자 DB', fns: ['MariaDB', 'MySQL', 'PostgreSQL'] },
}
export const HOST_LABEL: Record<string, string> = { tester: '계측기', target: '대상', mixed: '동거', empty: '빈 호스트' }
export type PaletteKind = 'host' | 'obs_ssh' | 'worker' | 'ue' | 'real-ue' | 'ibcf' | 'pbx' | 'mgcf' | 'n_sip' | 'n_tas' | 'n_media' | 'n_subscriber' | 'n_oam' | 'n_db'
export const POOL_KINDS: PaletteKind[] = ['ue', 'real-ue', 'ibcf', 'pbx', 'mgcf']

export const deep = <T,>(o: T): T => JSON.parse(JSON.stringify(o))

export function ensureLayout(doc: TopologyDoc): TopoLayout {
  if (!doc.layout) doc.layout = { regions: {}, items: {} }
  doc.layout.regions = doc.layout.regions ?? {}
  doc.layout.items = doc.layout.items ?? {}
  return doc.layout
}

export const hosts = (d: TopologyDoc) => Object.entries(d.hosts ?? {})
export const nodes = (d: TopologyDoc) => Object.entries(d.target?.nodes ?? {})
export const worker = (d: TopologyDoc, name: string) => (d.workers ?? []).find(w => w.name === name)
export const ipOfHost = (d: TopologyDoc, hid: string) => d.hosts?.[hid]?.ip ?? ''
export const ipOfWorker = (d: TopologyDoc, name: string) => { const w = worker(d, name); return w ? ipOfHost(d, w.host) : '' }
/** 노드 주소 — addr(VIP) → 호스트 ip */
export const ipOfNode = (d: TopologyDoc, nid: string) => { const n = d.target?.nodes?.[nid]; return n ? (n.addr || ipOfHost(d, n.host)) : '' }
/** 수신점 주소 — listener.ip → 노드 addr → 호스트 ip */
export const ipOfListener = (d: TopologyDoc, nid: string, lid: string) => { const l = d.target?.nodes?.[nid]?.sip?.listeners?.[lid]; return (l?.ip) || ipOfNode(d, nid) }
/** 피어 풀 수신점 ip — bind.ip → 워커 호스트 ip */
export const ipOfBind = (d: TopologyDoc, p: PeerPoolDoc) => p.bind?.ip || ipOfWorker(d, p.worker)
export const listenersOf = (n: TopoNode | undefined): [string, SipListener][] => Object.entries(n?.sip?.listeners ?? {})
export const accessListeners = (n: TopoNode | undefined) => listenersOf(n).filter(([, l]) => (l.edge ?? 'access') === 'access')
export const peeringListeners = (n: TopoNode | undefined) => listenersOf(n).filter(([, l]) => l.edge === 'peering')
/** cims 대상에서 이 수신점이 뜻하는 local_nodes 이름 */
export const localNodeName = (lid: string, l: SipListener) => l.local_node || `cims-tester-${lid}`
/** 풀이 닿는 수신점 id — 컨트롤러 `Topology.pool_listener` 와 같은 규칙. 없으면 null */
export function poolListener(d: TopologyDoc, p: PoolDoc): string | null {
  const n = d.target?.nodes?.[isPeer(p) ? p.peering : p.access]; if (!n?.sip?.listeners) return null
  if (p.listener && n.sip.listeners[p.listener]) return p.listener
  if (p.listener) return null
  if (isPeer(p)) {
    const proto = p.bind?.protocol ?? 'udp'
    return peeringListeners(n)[0]?.[0] ?? listenersOf(n).find(([, l]) => (l.protocol ?? 'udp') === proto)?.[0] ?? listenersOf(n)[0]?.[0] ?? null
  }
  return accessListeners(n).find(([, l]) => (l.protocol ?? 'udp') === (p.transport ?? 'udp'))?.[0] ?? null
}
/** UE 풀의 실효 transport — listener 가 있으면 그 protocol */
export function poolTransport(d: TopologyDoc, p: UePoolDoc | { kind: 'real-ue'; access: string; listener?: string; transport?: Transport }): Transport {
  const l = p.listener ? d.target?.nodes?.[p.access]?.sip?.listeners?.[p.listener] : undefined
  return (l?.protocol ?? p.transport ?? 'udp') as Transport
}
export function hostKind(d: TopologyDoc, hid: string): 'tester' | 'target' | 'mixed' | 'empty' {
  const ws = (d.workers ?? []).some(w => w.host === hid), ns = nodes(d).some(([, n]) => n.host === hid)
  return ws && ns ? 'mixed' : ws ? 'tester' : ns ? 'target' : 'empty'
}
export const colorOf = (d: TopologyDoc, hid: string) => (hosts(d).findIndex(([h]) => h === hid) % 6) + 1
/** UE 풀이 붙을 수 있는 노드 = access 수신점이 하나라도 있는 SIP 노드 */
export const accessNodes = (d: TopologyDoc) => nodes(d).filter(([, n]) => n.role === 'sip' && accessListeners(n).length).map(([id]) => id)
/** 피어 풀이 붙을 수 있는 노드 = 수신점이 하나라도 있는 SIP 노드(edge 무관 — CSP 는 Route 로 피어를 신뢰). 피어링 수신점 있는 노드가 앞 */
export const peeringNodes = (d: TopologyDoc) => nodes(d).filter(([, n]) => n.role === 'sip' && listenersOf(n).length).sort(([, a], [, b]) => Number(!!peeringListeners(b).length) - Number(!!peeringListeners(a).length)).map(([id]) => id)
export const dbNodes = (d: TopologyDoc) => nodes(d).filter(([, n]) => n.role === 'db' || (n.role === 'subscriber' && n.api)).map(([id]) => id)
export const mediaNodes = (d: TopologyDoc) => nodes(d).filter(([, n]) => n.role === 'media').map(([id]) => id)
export const isPeer = (p: PoolDoc): p is PeerPoolDoc => p.kind === 'peer'
export const isUe = (p: PoolDoc): p is UePoolDoc => p.kind === 'ue'
export function poolSize(p: PoolDoc): number {
  if (isPeer(p)) { const rg = p.identities?.e164_range ?? p.identities?.did_range; if (!rg) return 0; const n = parseInt(rg[1].replace(/\D/g, ''), 10) - parseInt(rg[0].replace(/\D/g, ''), 10) + 1; return p.identities.count ? Math.min(n, p.identities.count) : n }
  return ('count' in p.source ? p.source.count : undefined) ?? 0
}

export function validate(d: TopologyDoc): Issue[] {
  const out: Issue[] = []
  const add = (level: Level, who: string, msg: string, focus: Focus | null) => out.push({ level, who, msg, focus })
  const T = d.target ?? { name: '', nodes: {} }
  if (!(d.name ?? '').trim()) add('err', 'topology', '이름이 비었습니다', null)
  const ips = new Map<string, string>()
  for (const [id, h] of hosts(d)) {
    if (!ID_RE.test(id)) add('err', `hosts.${id}`, '호스트 id 는 소문자·숫자·_ 만', { kind: 'host', id })
    if (!h.ip) add('err', `hosts.${id}`, '호스트 주소가 비었습니다', { kind: 'host', id })
    else if (ips.has(h.ip)) add('warn', `hosts.${id}`, `주소 ${h.ip} 가 ${ips.get(h.ip)} 와 겹칩니다`, { kind: 'host', id }); else ips.set(h.ip, id)
    const ws = (d.workers ?? []).filter(w => w.host === id), ns = nodes(d).filter(([, n]) => n.host === id)
    if (ws.length && ns.length) add('info', `hosts.${id}`, `동거 호스트 — 워커 ${ws.map(w => w.name).join(',')} 와 대상 노드 ${ns.map(([nid, n]) => n.label ?? nid).join(',')} 가 같은 서버. CPU 지표는 워커 몫을 뺀 값으로 봅니다`, { kind: 'host', id })
    if (!ws.length && !ns.length) add('info', `hosts.${id}`, '빈 호스트 — 워커나 대상 노드를 놓으십시오', { kind: 'host', id })
    if (h.ssh && (!h.ssh.user || !h.ssh.key_env)) add('err', `hosts.${id}`, 'SSH 관측은 user 와 key_env 가 필요합니다', { kind: 'host', id })
    if (!h.ssh && ns.some(([, n]) => (n.procs ?? []).length)) add('warn', `hosts.${id}`, '노드에 감시 프로세스가 적혀 있지만 호스트에 SSH 관측이 없습니다 — CPU stop_on 을 못 씁니다', { kind: 'host', id })
  }
  if (!(d.workers ?? []).length) add('err', 'workers', '워커가 없습니다 — 호스트 위에 [워커]를 놓으십시오', null)
  const wn = new Set<string>()
  for (const w of d.workers ?? []) {
    if (!ID_RE.test(w.name)) add('err', `workers.${w.name}`, '워커 이름은 소문자·숫자·_ 만', { kind: 'worker', id: w.name })
    if (wn.has(w.name)) add('err', `workers.${w.name}`, '워커 이름이 중복됩니다', { kind: 'worker', id: w.name }); wn.add(w.name)
    if (!d.hosts?.[w.host]) add('err', `workers.${w.name}`, '호스트가 없습니다 — 호스트 영역 안으로 옮기십시오', { kind: 'worker', id: w.name })
    if (!Object.values(d.pools ?? {}).some(p => p.worker === w.name)) add('info', `workers.${w.name}`, '풀이 없는 워커 — run 에 참여하지 않습니다', { kind: 'worker', id: w.name })
  }
  if (!nodes(d).length) add('err', 'target.nodes', '대상 노드가 없습니다 — 호스트 위에 SIP 서버부터 놓으십시오', null)
  else if (!accessNodes(d).length) add('err', 'target.nodes', 'UE 가 접속할 access 수신점이 있는 SIP 노드가 없습니다', null)
  const listenerKeys = new Map<string, string>()
  for (const [id, n] of nodes(d)) {
    if (!ID_RE.test(id)) add('err', `nodes.${id}`, '노드 id 는 소문자·숫자·_ 만', { kind: 'node', id })
    if (!d.hosts?.[n.host]) add('err', `nodes.${id}`, '호스트가 없습니다 — 호스트 위로 옮기십시오', { kind: 'node', id })
    if (n.role === 'oam' && (T.kind ?? 'cims') !== 'cims') add('warn', `nodes.${id}`, 'OAM 노드는 CIMS 대상에서만 동작합니다', { kind: 'node', id })
    if (n.addr !== undefined && !n.addr) add('err', `nodes.${id}`, '노드 주소(addr)가 비었습니다 — 지우면 호스트 ip 를 씁니다', { kind: 'node', id })
    if (n.role === 'sip' && !listenersOf(n).length && !(n.procs ?? []).length) add('info', `nodes.${id}`, '수신점도 감시 프로세스도 없는 SIP 노드 — 그림에만 있습니다', { kind: 'node', id })
    for (const [lid, l] of listenersOf(n)) {
      if (!ID_RE.test(lid)) add('err', `nodes.${id}`, `수신점 id ${lid} — 소문자·숫자·_ 만`, { kind: 'node', id })
      if (!l.port) add('err', `nodes.${id}`, `수신점 ${lid} 의 포트가 비었습니다`, { kind: 'node', id })
      if (l.ip !== undefined && !l.ip) add('err', `nodes.${id}`, `수신점 ${lid} 의 ip 가 비었습니다 — 지우면 노드 주소를 씁니다`, { kind: 'node', id })
      const k = `${ipOfListener(d, id, lid)}:${l.port}/${l.protocol ?? 'udp'}`
      if (listenerKeys.has(k)) add('err', `nodes.${id}`, `수신점 ${lid} (${k}) 이 ${listenerKeys.get(k)} 과 겹칩니다`, { kind: 'node', id }); else listenerKeys.set(k, `${id}:${lid}`)
    }
    if (n.role === 'media' && !n.media?.rtp_range) add('warn', `nodes.${id}`, 'RTP 포트 범위가 없으면 미디어 지표를 이 노드에 귀속할 수 없습니다', { kind: 'node', id })
    if (n.role === 'subscriber' && !n.api) add('info', `nodes.${id}`, 'API 없는 가입자 서버 — UE 풀 원천은 creds 만 가능', { kind: 'node', id })
  }
  const bindKeys = new Map<string, string>()
  const pools = Object.entries(d.pools ?? {})
  if (!pools.length) add('err', 'pools', '풀이 없습니다 — 워커 위에 UE/피어 풀을 놓으십시오', null)
  for (const [pn, p] of pools) {
    if (!ID_RE.test(pn)) add('err', `pools.${pn}`, '풀 이름은 소문자·숫자·_ 만 (시나리오 roles.pool 이 참조)', { kind: 'pool', id: pn })
    const w = worker(d, p.worker)
    if (!w) add('err', `pools.${pn}`, '워커 위에 있지 않습니다 — 워커 카드로 끌어 놓으십시오', { kind: 'pool', id: pn })
    if (p.group !== undefined && p.group !== '' && !ID_RE.test(p.group)) add('err', `pools.${pn}`, 'group 은 소문자·숫자·_ 만 (시나리오가 참조하는 논리 풀 이름)', { kind: 'pool', id: pn })
    if (p.group && d.pools[p.group]) add('err', `pools.${pn}`, `group ${p.group} 이 다른 풀의 이름과 같습니다 — 역할 해석이 모호해집니다`, { kind: 'pool', id: pn })
    if (p.group && pools.some(([q, o]) => q !== pn && o.group === p.group && o.worker === p.worker)) add('err', `pools.${pn}`, `워커 ${p.worker} 에 group ${p.group} 풀이 둘 — 워커마다 논리 풀 하나만`, { kind: 'pool', id: pn })
    if (!isPeer(p)) {
      const a = T.nodes?.[p.access]
      if (!a || !accessListeners(a).length) add('err', `pools.${pn}`, `접속점 노드 ${p.access || '(없음)'} 이 없거나 access 수신점이 없습니다 — SIP 서버의 수신점 행으로 끌어 놓으십시오`, { kind: 'pool', id: pn })
      else if (p.listener && !a.sip?.listeners?.[p.listener]) add('err', `pools.${pn}`, `수신점 ${p.listener} 이 ${p.access} 에 없습니다`, { kind: 'pool', id: pn })
      else if (p.listener && (a.sip!.listeners![p.listener].edge ?? 'access') !== 'access') add('err', `pools.${pn}`, `수신점 ${p.listener} 은 access 가 아닙니다 — UE 는 access 수신점으로 등록합니다`, { kind: 'pool', id: pn })
      else if (!poolListener(d, p)) add('err', `pools.${pn}`, `transport ${p.transport ?? 'udp'} 인데 ${p.access} 에 ${p.transport ?? 'udp'} access 수신점이 없습니다`, { kind: 'pool', id: pn })
      if ('db' in p.source) { if (!dbNodes(d).includes(p.source.db)) add('err', `pools.${pn}`, `원천 노드 ${p.source.db || '(없음)'} 가 없거나 DB/API 가 아닙니다`, { kind: 'pool', id: pn }); if (!p.source.count) add('err', `pools.${pn}`, 'DB 원천은 count 가 필수입니다', { kind: 'pool', id: pn }) }
      if ('creds' in p.source && !p.source.creds) add('err', `pools.${pn}`, 'creds 경로가 비었습니다', { kind: 'pool', id: pn })
      if (p.srtp === 'required' && p.transport !== 'tls') add('warn', `pools.${pn}`, 'srtp required 는 TLS 접속에서만 협상됩니다', { kind: 'pool', id: pn })
    } else {
      const ip = w ? ipOfBind(d, p) : '?'; const k = `${ip}:${p.bind?.port}/${p.bind?.protocol ?? 'udp'}`
      if (p.bind?.ip !== undefined && !p.bind.ip) add('err', `pools.${pn}`, 'bind.ip 가 비었습니다 — 지우면 워커 호스트 ip 를 씁니다', { kind: 'pool', id: pn })
      if (bindKeys.has(k)) add('err', `pools.${pn}`, `수신점 ${k} 이 ${bindKeys.get(k)} 과 겹칩니다`, { kind: 'pool', id: pn }); else bindKeys.set(k, pn)
      const pg = T.nodes?.[p.peering]
      if (!pg || !listenersOf(pg).length) add('err', `pools.${pn}`, `다음 홉 노드 ${p.peering || '(없음)'} 이 없거나 수신점이 없습니다 — SIP 서버의 수신점 행으로 끌어 놓으십시오`, { kind: 'pool', id: pn })
      else if (p.listener && !pg.sip?.listeners?.[p.listener]) add('err', `pools.${pn}`, `수신점 ${p.listener} 이 ${p.peering} 에 없습니다`, { kind: 'pool', id: pn })
      else { const lid = poolListener(d, p)!; const l = pg.sip!.listeners![lid]; if ((l.edge ?? 'access') !== 'peering') add('info', `pools.${pn}`, `access 수신점 ${lid} 을 다음 홉으로 씁니다 — CSP 는 시드된 Route 로 이 피어를 신뢰합니다(cims). 타 IMS 는 그쪽 설정이 필요합니다`, { kind: 'pool', id: pn }) }
      if (!p.domain) add('err', `pools.${pn}`, '피어 domain 이 비었습니다', { kind: 'pool', id: pn })
      const idn = p.identities ?? {}
      if (!idn.e164_range && !idn.did_range) add('err', `pools.${pn}`, '신원 범위(e164_range / did_range)가 필요합니다', { kind: 'pool', id: pn })
      if ((T.kind ?? 'cims') === 'cims') { if (!p.seed?.route_set && !p.seed?.acl) add('warn', `pools.${pn}`, 'route_set 도 acl 도 없습니다 — 시드는 풀 이름의 RouteSet 으로', { kind: 'pool', id: pn }) }
      else if (p.seed?.route_set || p.seed?.acl) add('warn', `pools.${pn}`, '타 IMS 대상에는 컬렉션 시드가 없습니다', { kind: 'pool', id: pn })
      if (p.register && pg && !accessListeners(pg).length) add('err', `pools.${pn}`, `트렁크 REGISTER 는 ${p.peering} 의 access 수신점으로 가는데 하나도 없습니다`, { kind: 'pool', id: pn })
      if (p.register && !p.register.ha1_env && !p.register.password_env) add('err', `pools.${pn}`, 'register 에 ha1_env 또는 password_env 하나는 필요합니다', { kind: 'pool', id: pn })
      if (p.profile === 'pbx' && !p.register) add('info', `pools.${pn}`, 'PBX 프로파일인데 register 가 없습니다 — 고정 IP 트렁크로 동작', { kind: 'pool', id: pn })
    }
  }
  if (!mediaNodes(d).length && pools.some(([, p]) => !isPeer(p))) add('info', 'target.nodes', '미디어 서버 노드가 없어 RTP 지표를 대상에 귀속하지 않습니다 (단말 측 측정만)', null)
  return out
}

// ── 이름 바꾸기 (참조 동시 갱신) ─────────────────────────────────────────────
function renameKey<T>(obj: Record<string, T>, a: string, b: string): Record<string, T> { const next: Record<string, T> = {}; for (const [k, v] of Object.entries(obj)) next[k === a ? b : k] = v; return next }
export function renameHost(d: TopologyDoc, a: string, b: string) {
  if (!b || a === b || d.hosts[b]) return false
  d.hosts = renameKey(d.hosts, a, b); const L = ensureLayout(d); if (L.regions[a]) { L.regions[b] = L.regions[a]; delete L.regions[a] }
  d.workers.forEach(w => { if (w.host === a) w.host = b }); nodes(d).forEach(([, n]) => { if (n.host === a) n.host = b }); return true
}
export function renameNode(d: TopologyDoc, a: string, b: string) {
  if (!b || a === b || d.target.nodes[b]) return false
  d.target.nodes = renameKey(d.target.nodes, a, b); const L = ensureLayout(d); if (L.items[a]) { L.items[b] = L.items[a]; delete L.items[a] }
  Object.values(d.pools).forEach(p => { if (!isPeer(p) && p.access === a) p.access = b; if (isPeer(p) && p.peering === a) p.peering = b; if (isUe(p) && 'db' in p.source && p.source.db === a) p.source.db = b }); return true
}
export function renamePool(d: TopologyDoc, a: string, b: string) { if (!b || a === b || d.pools[b]) return false; d.pools = renameKey(d.pools, a, b); return true }
export function renameWorker(d: TopologyDoc, a: string, b: string) {
  if (!b || a === b || worker(d, b)) return false
  const w = worker(d, a); if (!w) return false
  Object.values(d.pools).forEach(p => { if (p.worker === a) p.worker = b }); const L = ensureLayout(d); if (L.items[a]) { L.items[b] = L.items[a]; delete L.items[a] }; w.name = b; return true
}

// ── 팔레트 생성 ───────────────────────────────────────────────────────────────
export function uniq(d: TopologyDoc, base: string): string { let n = base, i = 2; while (d.pools[n] || d.target.nodes[n] || d.hosts[n] || worker(d, n)) n = `${base}_${i++}`; return n }
function nextIp(d: TopologyDoc, prefix: string): string { const used = new Set(hosts(d).map(([, h]) => h.ip)); for (let i = 50; i < 250; i++) { const ip = `${prefix}.${i}`; if (!used.has(ip)) return ip } return `${prefix}.250` }
export interface Pos { x: number; y: number }
export function relPos(d: TopologyDoc, host: string, pos: Pos): Pos {
  const r = ensureLayout(d).regions[host]; if (!r) return { x: pos.x, y: pos.y }
  return { x: Math.max(4, Math.round((pos.x - r.x - 20) / 10) * 10), y: Math.max(4, Math.round((pos.y - r.y - 30 - 10) / 10) * 10) }
}
export function newHost(d: TopologyDoc, pos: Pos): string {
  const id = uniq(d, 'host'); d.hosts[id] = { name: id, ip: nextIp(d, '10.0.0') }
  ensureLayout(d).regions[id] = { x: Math.max(20, pos.x - 30), y: Math.max(20, pos.y - 20), w: 300, h: 120 }; return id
}
export function newWorker(d: TopologyDoc, host: string, pos: Pos): string {
  const name = uniq(d, `w${d.workers.length + 1}`)
  d.workers.push({ name, host, port: 7100 + d.workers.filter(w => w.host === host).length, cpus: 8 }); ensureLayout(d).items[name] = relPos(d, host, pos); return name
}
/** 팔레트에서 놓기 — 빈 곳이면 담을 상자를 만든다(워커·노드 → 호스트, 풀 → 호스트+워커). 반환 = {sel, made[]} */
export function createFromPalette(d: TopologyDoc, kind: PaletteKind, pos: Pos, ctx: { host: string | null; worker: string | null }): { sel: Focus | null; made: string[] } {
  let { host, worker: wname } = ctx
  const made: string[] = []
  const isPool = POOL_KINDS.includes(kind)
  if (!host && kind !== 'host') { host = newHost(d, pos); made.push(`호스트 ${host}`) }
  if (isPool && !wname) { wname = newWorker(d, host!, pos); made.push(`워커 ${wname}`); pos = { x: pos.x + 8, y: pos.y + 8 } }
  if (kind === 'host') { const id = uniq(d, 'host'); d.hosts[id] = { name: id, ip: nextIp(d, '10.0.0') }; ensureLayout(d).regions[id] = { x: Math.max(20, pos.x - 40), y: Math.max(20, pos.y - 20), w: 360, h: 240 }; return { sel: { kind: 'host', id }, made } }
  if (kind === 'obs_ssh') { const h = d.hosts[host!]; if (!h.ssh) h.ssh = { user: 'cims', key_env: 'TESTER_SSH_KEY' }; return { sel: { kind: 'host', id: host! }, made } }
  if (kind === 'worker') { const name = newWorker(d, host!, pos); return { sel: { kind: 'worker', id: name }, made } }
  if (kind.startsWith('n_')) {
    const role = kind.slice(2) as NodeRole; const id = uniq(d, role)
    const base: TopoNode = { role, fn: ROLE[role].fns[0], label: ROLE[role].label, host: host!, procs: [] }
    if (role === 'sip') base.sip = { domains: ['ims.example.kr'], listeners: { udp: { edge: 'access', port: 5060, protocol: 'udp' }, tcp: { edge: 'access', port: 5060, protocol: 'tcp' }, tls: { edge: 'access', port: 5061, protocol: 'tls' } } }
    if (role === 'media') base.media = { rtp_range: [10000, 19999] }
    if (role === 'subscriber') base.api = { port: 4430, tls: true }
    if (role === 'oam') base.oam = { port: 4419, tls: true, token_env: 'TESTER_OAM_TOKEN', observe: ['oam_stats', 'oam_alarms'] }
    if (role === 'db') base.db = { port: 3306, name: 'cims', user_env: 'TESTER_DB_USER', password_env: 'TESTER_DB_PASS' }
    if (role === 'tas') base.tas = { port: 5060 }
    d.target.nodes[id] = base; ensureLayout(d).items[id] = relPos(d, host!, pos); return { sel: { kind: 'node', id }, made }
  }
  if (kind === 'ue' || kind === 'real-ue') {
    const name = uniq(d, kind === 'ue' ? 'ue_pool' : 'real_ue'); const acc = accessNodes(d)[0] ?? ''
    const p: PoolDoc = kind === 'ue' ? { kind: 'ue', worker: wname!, access: acc, source: { creds: `creds/${name}.jsonl` }, transport: 'udp', srtp: 'off' }
      : { kind: 'real-ue', worker: wname!, access: acc, source: { creds: `creds/${name}.jsonl` }, transport: 'tls', srtp: 'optional' }
    const al = accessListeners(d.target.nodes[acc]); if (al.length && !al.some(([, l]) => (l.protocol ?? 'udp') === p.transport)) p.transport = (al[0][1].protocol ?? 'udp') as Transport
    d.pools[name] = p; return { sel: { kind: 'pool', id: name }, made }
  }
  const name = uniq(d, kind === 'pbx' ? `pbx_${wname}` : `${kind}_peer`)
  const usedPorts = Object.values(d.pools).filter(p => isPeer(p) && p.worker === wname).map(p => (p as PeerPoolDoc).bind.port); let port = 5080; while (usedPorts.includes(port)) port++
  const cims = (d.target.kind ?? 'cims') === 'cims'
  const p: PeerPoolDoc = { kind: 'peer', worker: wname!, peering: peeringNodes(d)[0] ?? '', profile: kind as 'ibcf' | 'pbx' | 'mgcf', bind: { port, protocol: 'udp' }, domain: `${kind}.${name.replace(/_/g, '-')}.test`,
    identities: kind === 'pbx' ? { did_range: ['0212345000', '0212345099'], ext_len: 4 } : { e164_range: ['+82200000000', '+82200000099'] },
    seed: cims ? { priority: 100, distribution: 'failover', route_set: `rs-${name.replace(/_/g, '-')}` } : {} }
  if (kind === 'pbx') p.register = { user: name, ha1_env: `${name.toUpperCase()}_HA1` }
  d.pools[name] = p; return { sel: { kind: 'pool', id: name }, made }
}
export function deleteFocus(d: TopologyDoc, f: Focus) {
  const L = ensureLayout(d)
  if (f.kind === 'host') { delete d.hosts[f.id]; delete L.regions[f.id] }
  if (f.kind === 'worker') { d.workers = d.workers.filter(w => w.name !== f.id); delete L.items[f.id] }
  if (f.kind === 'node') { delete d.target.nodes[f.id]; delete L.items[f.id] }
  if (f.kind === 'pool') delete d.pools[f.id]
}
export function refsOf(d: TopologyDoc, f: Focus): string[] {
  if (f.kind === 'host') return [...d.workers.filter(w => w.host === f.id).map(w => `워커 ${w.name}`), ...nodes(d).filter(([, n]) => n.host === f.id).map(([id]) => `노드 ${id}`)]
  if (f.kind === 'worker') return Object.entries(d.pools).filter(([, p]) => p.worker === f.id).map(([n]) => `풀 ${n}`)
  if (f.kind === 'node') return Object.entries(d.pools).filter(([, p]) => (!isPeer(p) && p.access === f.id) || (isPeer(p) && p.peering === f.id) || (isUe(p) && 'db' in p.source && p.source.db === f.id)).map(([n]) => `풀 ${n}`)
  return []
}

// ── 프리셋 (CIMS 한 호스트 5 노드 · 일반 IMS 분리 배치 · IP-PBX) ────────────────
const TESTER_HOSTS = { h61: { name: 'tester-a', ip: '10.0.0.61' }, h62: { name: 'tester-b', ip: '10.0.0.62' } }
const WORKERS = [{ name: 'w1', host: 'h61', port: 7100, cpus: 8 }, { name: 'w2', host: 'h62', port: 7100, cpus: 8 }]
export const PRESETS: Record<'cims' | 'ims' | 'pbx', { name: string; hosts: TopologyDoc['hosts']; nodes: Record<string, TopoNode>; pools: Record<string, PoolDoc>; layout: TopoLayout }> = {
  cims: { name: 'media01', hosts: { h45: { name: 'media01', ip: '10.0.0.45', ssh: { user: 'cims', key_env: 'TESTER_SSH_KEY' } } },
    nodes: {
      csp: { role: 'sip', fn: 'CSP', label: 'CSP', host: 'h45', procs: ['csp'], sip: { domains: ['volte.cims.example.kr', 'ptt.cims.example.kr'], listeners: { udp: { edge: 'access', port: 5060, protocol: 'udp' }, tcp: { edge: 'access', port: 25061, protocol: 'tcp' }, tls: { edge: 'access', port: 5061, protocol: 'tls' }, peering: { edge: 'peering', port: 5070, protocol: 'udp', local_node: 'cims-tester-peering' } } } },
      cmp: { role: 'media', fn: 'CMP', label: 'CMP', host: 'h45', procs: ['cmp'], media: { rtp_range: [20000, 29999], control: 9001 } },
      csc: { role: 'subscriber', fn: 'CSC', label: 'CSC', host: 'h45', procs: ['csc'], api: { port: 4430, tls: true } },
      oam: { role: 'oam', fn: 'OAM', label: 'OAM', host: 'h45', procs: ['oam'], oam: { port: 4419, tls: true, token_env: 'TESTER_OAM_TOKEN', observe: ['oam_stats', 'oam_alarms', 'agent_heartbeat'] } },
      db: { role: 'db', fn: 'MariaDB', label: '가입자 DB', host: 'h45', procs: ['mariadbd'], db: { port: 3306, name: 'cims', user_env: 'TESTER_DB_USER', password_env: 'TESTER_DB_PASS' } },
    },
    pools: {
      volte_ue_a: { kind: 'ue', worker: 'w1', group: 'volte_ue', access: 'csp', source: { creds: 'creds/volte-a.jsonl' }, transport: 'tls', srtp: 'optional' },
      volte_ue_b: { kind: 'ue', worker: 'w2', group: 'volte_ue', access: 'csp', source: { creds: 'creds/volte-b.jsonl' }, transport: 'tls', srtp: 'optional' },
      ptt_ue: { kind: 'ue', worker: 'w1', access: 'csp', source: { creds: 'creds/ptt.jsonl' }, transport: 'udp', srtp: 'off' },
      peer_kt: { kind: 'peer', worker: 'w2', peering: 'csp', profile: 'ibcf', bind: { port: 5080, protocol: 'udp' }, domain: 'ims.kt.test', identities: { e164_range: ['+82212340000', '+82212349999'], count: 200 }, seed: { route_set: 'rs-kt', priority: 100, distribution: 'failover' } },
      peer_kt_dead: { kind: 'peer', worker: 'w2', peering: 'csp', profile: 'ibcf', bind: { port: 5081, protocol: 'udp' }, domain: 'ims.kt.test', answer: 'silent', identities: { e164_range: ['+82212340000', '+82212340009'] }, seed: { route_set: 'rs-kt', priority: 50, distribution: 'failover' } },
      pbx_hq: { kind: 'peer', worker: 'w1', peering: 'csp', profile: 'pbx', bind: { port: 5090, protocol: 'udp' }, domain: 'pbx.hq.test', register: { user: 'pbx-hq', ha1_env: 'PBX_HA1' }, identities: { did_range: ['0212345000', '0212345099'], ext_len: 4 }, seed: { route_set: 'rs-pbx', priority: 100 } },
    },
    layout: { regions: { h45: { x: 420, y: 40, w: 990, h: 470 } }, items: { csp: { x: 14, y: 12 }, cmp: { x: 290, y: 12 }, csc: { x: 560, y: 12 }, oam: { x: 290, y: 150 }, db: { x: 560, y: 150 } } } },
  ims: { name: 'ims-lab', hosts: { hp: { name: 'pcscf-1', ip: '10.1.0.11' }, hs: { name: 'scscf-1', ip: '10.1.0.12' }, hb: { name: 'ibcf-1', ip: '10.1.0.13' }, ht: { name: 'tas-1', ip: '10.1.0.14' }, hm: { name: 'mrf-1', ip: '10.1.0.15' }, hh: { name: 'hss-1', ip: '10.1.0.16' } },
    nodes: {
      pcscf: { role: 'sip', fn: 'P-CSCF', label: 'P-CSCF', host: 'hp', procs: ['pcscf'], sip: { domains: ['ims.mnc001.mcc450.3gppnetwork.org'], listeners: { udp: { edge: 'access', port: 5060, protocol: 'udp' }, tcp: { edge: 'access', port: 5060, protocol: 'tcp' }, tls: { edge: 'access', port: 5061, protocol: 'tls' } } } },
      scscf: { role: 'sip', fn: 'S-CSCF', label: 'I/S-CSCF', host: 'hs', procs: ['scscf', 'icscf'], sip: {} },
      ibcf: { role: 'sip', fn: 'IBCF', label: 'IBCF', host: 'hb', procs: [], sip: { listeners: { peering: { edge: 'peering', port: 5060, protocol: 'udp' } } } },
      tas: { role: 'tas', fn: 'TAS', label: 'TAS', host: 'ht', procs: ['tas'], tas: { port: 5060 } },
      mrf: { role: 'media', fn: 'MRF', label: 'MRF', host: 'hm', procs: [], media: { rtp_range: [10000, 19999] } },
      hss: { role: 'subscriber', fn: 'HSS', label: 'HSS', host: 'hh', procs: [] },
    },
    pools: {
      volte_ue: { kind: 'ue', worker: 'w1', access: 'pcscf', source: { creds: 'creds/ims-lab.jsonl', count: 500 }, transport: 'udp', srtp: 'off' },
      peer_pstn: { kind: 'peer', worker: 'w2', peering: 'ibcf', profile: 'mgcf', bind: { port: 5080, protocol: 'udp' }, domain: 'pstn.gw.test', identities: { e164_range: ['+82312340000', '+82312340099'] }, seed: {} },
    },
    layout: { regions: { hp: { x: 420, y: 40, w: 300, h: 200 }, hs: { x: 740, y: 40, w: 300, h: 200 }, hb: { x: 1060, y: 40, w: 300, h: 200 }, ht: { x: 420, y: 270, w: 300, h: 200 }, hm: { x: 740, y: 270, w: 300, h: 200 }, hh: { x: 1060, y: 270, w: 300, h: 200 } },
              items: { pcscf: { x: 14, y: 12 }, scscf: { x: 14, y: 12 }, ibcf: { x: 14, y: 12 }, tas: { x: 14, y: 12 }, mrf: { x: 14, y: 12 }, hss: { x: 14, y: 12 } } } },
  pbx: { name: 'pbx-hq', hosts: { hx: { name: 'pbx-hq', ip: '10.2.0.20' } },
    nodes: {
      pbx: { role: 'sip', fn: 'PBX', label: 'IP-PBX', host: 'hx', procs: [], sip: { domains: ['pbx.example.kr'], listeners: { udp: { edge: 'access', port: 5060, protocol: 'udp' }, tcp: { edge: 'access', port: 5060, protocol: 'tcp' } } } },
      pbxmedia: { role: 'media', fn: 'PBX 미디어', label: 'PBX RTP', host: 'hx', procs: [], media: { rtp_range: [16384, 32767] } },
    },
    pools: {
      ext_ue: { kind: 'ue', worker: 'w1', access: 'pbx', source: { creds: 'creds/pbx-ext.jsonl' }, transport: 'udp', srtp: 'off' },
      trunk_in: { kind: 'peer', worker: 'w2', peering: 'pbx', profile: 'ibcf', bind: { port: 5080, protocol: 'udp' }, domain: 'carrier.test', identities: { e164_range: ['+82212340000', '+82212340099'] }, seed: {} },
    },
    layout: { regions: { hx: { x: 420, y: 40, w: 600, h: 230 } }, items: { pbx: { x: 14, y: 12 }, pbxmedia: { x: 290, y: 12 } } } },
}
/** 프리셋 적용 — 워커가 있는 호스트·워커는 유지하고 대상 노드·풀(과 대상 전용 호스트)만 바꾼다. keep=null 이면 계측기 호스트도 기본값 */
export function fromPreset(k: keyof typeof PRESETS, keep: TopologyDoc | null): TopologyDoc {
  const P = PRESETS[k]
  const keepIds = keep ? hosts(keep).filter(([id]) => hostKind(keep, id) !== 'target').map(([id]) => id) : []
  const kHosts = keep ? Object.fromEntries(hosts(keep).filter(([id]) => keepIds.includes(id))) : deep(TESTER_HOSTS)
  const kWorkers = keep ? keep.workers.filter(w => keepIds.includes(w.host)) : deep(WORKERS)
  const KL = keep ? ensureLayout(keep) : null
  const doc: TopologyDoc = {
    name: keep?.name || P.name, hosts: { ...kHosts, ...deep(P.hosts) }, workers: kWorkers,
    target: { name: P.name, kind: k, nodes: deep(P.nodes) },
    pools: Object.fromEntries(Object.entries(deep(P.pools)).filter(([, p]) => kWorkers.some(w => w.name === p.worker))),
    layout: { regions: { h61: { x: 30, y: 40, w: 300, h: 120 }, h62: { x: 30, y: 400, w: 300, h: 120 }, ...(KL ? Object.fromEntries(Object.entries(KL.regions).filter(([id]) => keepIds.includes(id))) : {}), ...deep(P.layout.regions) },
              items: { w1: { x: 14, y: 12 }, w2: { x: 14, y: 12 }, ...(KL ? Object.fromEntries(Object.entries(KL.items).filter(([id]) => kWorkers.some(w => w.name === id))) : {}), ...deep(P.layout.items) } },
  }
  return doc
}

/** 자동 배치 — 호스트 안 카드를 격자로, 호스트 영역은 계측기 열 / 대상 열로 */
export function autoLayout(d: TopologyDoc, sizes: (id: string) => { w: number; h: number }) {
  const L = ensureLayout(d)
  for (const [id] of hosts(d)) {
    const items = [...nodes(d).filter(([, n]) => n.host === id).map(([nid]) => ({ id: nid, ...sizes(nid) })), ...d.workers.filter(w => w.host === id).map(w => ({ id: w.name, ...sizes(w.name) }))]
    const cols = Math.max(1, Math.min(3, items.length))
    let x = 14, y = 12, rowH = 0, col = 0, right = 0, bottom = 0
    items.forEach(i => { if (col === cols) { col = 0; x = 14; y += rowH + 12; rowH = 0 } L.items[i.id] = { x, y }; right = Math.max(right, x + i.w); bottom = Math.max(bottom, y + i.h); x += i.w + 20; rowH = Math.max(rowH, i.h); col++ })
    const r = (L.regions[id] ||= { x: 40, y: 40, w: 360, h: 240 }); r.w = Math.max(300, right + 20); r.h = Math.max(110, bottom + 30 + 14)
  }
  const colsX = [30, 420, 860]; const cy = [40, 40, 40]
  hosts(d).forEach(([id]) => { const r = L.regions[id]; let c = hostKind(d, id) === 'target' ? 1 : 0; if (c === 1 && cy[1] > cy[2] + 200) c = 2; r.x = colsX[c]; r.y = cy[c]; cy[c] += r.h + 24 })
}
