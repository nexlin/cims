// 서비스 설정 › SIP 연동 — CSP 연동·규칙 컬렉션(local/remote node · route · route set · rule · rule set ·
// routing/acl policy)을 한 화면에서 편집하기 위한 읽기·묶음 저장.
//   읽기 = GET /deployments/{id}/collection/{name} (컬렉션마다)
//   저장 = 바뀐 컬렉션만 — 추가·변경은 참조되는 쪽 먼저, 삭제는 참조하는 쪽 먼저(등록의 반대) PUT(signal=false) → 마지막 하나만 signal=true
//          (agent 가 jsonl 만 쓰고 CSP 는 SIGUSR1 에서 9종 전부를 한 번에 다시 읽는다 — sip_runtime_config.md)
//   충돌 = 쓰기 직전에 다시 GET 해서 읽을 때와 달라졌으면 멈춘다 (마지막 쓰기 승리 방지)
import { deploymentApi, type Deployment } from '@core/api/deployment'

export type Rec = Record<string, any>

// 참조 순서 — 참조되는 쪽이 먼저. access_services 는 이 화면에서 읽기만 한다.
export const WRITE_ORDER = ['remote_nodes', 'routes', 'route_sets', 'rules', 'rule_sets',
                            'routing_policies', 'acl_policies'] as const
export type WCol = typeof WRITE_ORDER[number]
export const READ_COLS = ['local_nodes', ...WRITE_ORDER, 'access_services'] as const
export type Col = typeof READ_COLS[number]
export type Data = Record<Col, Rec[]>

export async function listCspDeployments(): Promise<Deployment[]> {
  const all = await deploymentApi.listDeployments()
  return all.filter(d => (d.process_name ?? d.package_name ?? '').toLowerCase() === 'csp' && d.status !== 'removed')
}

export async function loadAll(depId: number): Promise<Data> {
  const out = {} as Data
  await Promise.all(READ_COLS.map(async c => {
    const r = await deploymentApi.getDeploymentCollection(depId, c)
    out[c] = r.records ?? []
  }))
  return out
}

/** CSP Setup.Roles.IBCF — false 면 Route Set 으로 가는 Routing Policy 판정이 전부 403 이다. */
export async function loadIbcfRole(depId: number): Promise<boolean | null> {
  try {
    const cfg = await deploymentApi.getDeploymentConfig(depId)
    const v = (cfg.effective ?? {})['Setup.Roles.IBCF']
    return v ? Boolean(v.v) : null
  } catch { return null }
}

const canon = (rs: Rec[]) => JSON.stringify([...rs].sort((a, b) => String(a.name).localeCompare(String(b.name))))

export function changedCols(base: Data, cur: Data): WCol[] {
  return WRITE_ORDER.filter(c => canon(base[c]) !== canon(cur[c]))
}

export interface SaveResult { written: WCol[]; conflict?: WCol }

export async function saveAll(depId: number, base: Data, cur: Data): Promise<SaveResult> {
  const cols = changedCols(base, cur)
  // 충돌 검사 — 쓰기 전에 전부 확인한다 (중간에 멈추면 반쯤 쓴 상태가 남는다)
  for (const c of cols) {
    const now = await deploymentApi.getDeploymentCollection(depId, c)
    if (canon(now.records ?? []) !== canon(base[c])) return { written: [], conflict: c }
  }
  // 두 단계로 쓴다 — 중간에 끊겨도 디스크의 파일끼리 참조가 어긋나지 않게.
  //   1단계 (추가·변경, 참조되는 쪽 먼저): 새 레코드를 넣되 지울 레코드는 아직 남긴다
  //   2단계 (삭제, 참조하는 쪽 먼저 = 등록의 반대 순서): 지울 레코드를 뺀 최종본
  //   CSP 재적재 신호는 맨 마지막 쓰기 한 번 — CSP 는 그때 전부를 한 번에 읽는다
  const names = (rs: Rec[]) => new Set(rs.map(r => String(r.name)))
  const writes: Array<[WCol, Rec[]]> = []
  const delCols = new Set<WCol>()
  for (const c of cols) {
    const keep = names(cur[c])
    const gone = base[c].filter(r => !keep.has(String(r.name)))
    if (gone.length) { delCols.add(c); const stage = [...cur[c], ...gone]
      if (canon(stage) !== canon(base[c])) writes.push([c, stage]) }   // 지우기만 하는 컬렉션은 1단계를 건너뛴다
    else writes.push([c, cur[c]])
  }
  for (const c of [...WRITE_ORDER].reverse()) if (delCols.has(c)) writes.push([c, cur[c]])
  for (let i = 0; i < writes.length; i++) {
    await deploymentApi.putDeploymentCollection(depId, writes[i][0], writes[i][1], i === writes.length - 1)
  }
  return { written: cols }
}

/** 레코드 단위 차이 — 미리보기용 (name 기준) */
export function diffCol(a: Rec[], b: Rec[]) {
  const am = new Map(a.map(r => [String(r.name), JSON.stringify(r)]))
  const bm = new Map(b.map(r => [String(r.name), JSON.stringify(r)]))
  const lines: Array<['add' | 'del', string]> = []
  let add = 0, chg = 0, del = 0
  bm.forEach((v, k) => {
    if (!am.has(k)) { add++; lines.push(['add', v]) }
    else if (am.get(k) !== v) { chg++; lines.push(['del', am.get(k)!]); lines.push(['add', v]) }
  })
  am.forEach((v, k) => { if (!bm.has(k)) { del++; lines.push(['del', v]) } })
  return { add, chg, del, lines }
}
