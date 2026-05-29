// 데이터 소스 레지스트리 — 서비스 manifest 의 dataSources 를 집계. shape 위젯이 선택지로 소비.
import { SERVICE_MANIFESTS } from '../../services/registry'
import type { DataSource, ShapeKind, SourceParams } from './types'

// 순환 import 상 모듈 eval 시점 SERVICE_MANIFESTS 부분 초기화 회피 — lazy 집계.
let _all: DataSource[] | null = null
let _byId: Map<string, DataSource> | null = null
function ensure() {
  if (_all && _byId) return
  _all = SERVICE_MANIFESTS.flatMap(
    m => (m.dataSources ?? []).map(s => ({ ...s, serviceId: s.serviceId ?? m.id }))
  )
  _byId = new Map(_all.map(s => [s.id, s]))
}

export function getSource(id: string): DataSource | undefined { ensure(); return _byId!.get(id) }
export function allSources(): DataSource[] { ensure(); return _all! }
export function sourcesForShape(shape: ShapeKind): DataSource[] {
  ensure()
  return _all!.filter(s => s.shapes.includes(shape))
}

// co-placed 위젯이 같은 소스를 쓰면 load 중복 방지 — (sourceId|date|gran) 키 단기 캐시.
const _cache = new Map<string, { ts: number; p: Promise<unknown> }>()
const TTL_MS = 4000

export function loadSource(src: DataSource, params: SourceParams): Promise<unknown> {
  const key = `${src.id}|${params.date}|${params.granularity}`
  const hit = _cache.get(key)
  const now = Date.now()
  if (hit && now - hit.ts < TTL_MS) return hit.p
  const p = src.load(params)
  _cache.set(key, { ts: now, p })
  // 실패한 promise 는 캐시에서 즉시 제거 → 다음 시도 재요청.
  p.catch(() => { if (_cache.get(key)?.p === p) _cache.delete(key) })
  return p
}
