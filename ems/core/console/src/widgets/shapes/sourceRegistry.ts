// 데이터 소스 카탈로그 — Service Descriptor 의 data_sources(데이터)를 백엔드에서 받아
// buildDataSource 로 DataSource 화. 소스는 코드가 아니라 descriptor 등록으로 추가된다.
// 모듈 싱글톤(한 번 fetch, 구독 알림) — 여러 shape 위젯이 카탈로그를 공유.
import { useEffect, useState } from 'react'
import { serviceDescriptorsApi } from '../../api/serviceDescriptors'
import { buildDataSource } from './dataSourceSpec'
import type { DataSource, ShapeKind, SourceParams } from './types'

let _catalog: DataSource[] | null = null
let _byId: Map<string, DataSource> = new Map()
let _loading = false
let _error = ''
let _started = false
const _subs = new Set<() => void>()

function _notify() { _subs.forEach(fn => fn()) }

async function _fetch() {
  _loading = true; _error = ''; _notify()
  try {
    const res = await serviceDescriptorsApi.dataSources()
    _catalog = (res.data_sources || []).map(buildDataSource)
    _byId = new Map(_catalog.map(s => [s.id, s]))
  } catch (e) {
    _error = (e as Error).message; _catalog = []; _byId = new Map()
  } finally { _loading = false; _notify() }
}

export function getSource(id: string): DataSource | undefined { return _byId.get(id) }
// 훅 밖(편집기 [⚙] 패널의 옵션 목록 등)에서 쓰는 동기 접근자 — 아직 미로딩이면 빈 배열.
export function catalogSources(): DataSource[] { return _catalog ?? [] }
export function sourcesForShape(shape: ShapeKind, catalog: DataSource[]): DataSource[] {
  // stat(지표 1개)은 kpi 계약을 쓴다 — descriptor 는 'kpi' 만 선언하므로 여기서 이어준다.
  const want: ShapeKind = shape === 'stat' ? 'kpi' : shape
  return catalog.filter(s => s.shapes.includes(want))
}

// 카탈로그 **구독만** 하는 훅 — 로드를 시작하지 않는다. 이미 누가(shape 위젯) 로드했을 때만 값이 온다.
// [API] 배지가 소스 id → endpoint 환산에 쓴다: 배지 때문에 카탈로그를 새로 받아오지는 않으면서
// (개발자 모드 OFF 평시 트래픽 0 원칙), 카탈로그가 나중에 도착하면 리렌더돼 배지가 살아난다.
export function useDataSourceCatalogPassive(): DataSource[] {
  const [, setTick] = useState(0)
  useEffect(() => {
    const sub = () => setTick(t => t + 1)
    _subs.add(sub)
    return () => { _subs.delete(sub) }
  }, [])
  return _catalog ?? []
}

// 카탈로그 구독 훅 — 첫 사용 시 1회 fetch. reload() 로 강제 갱신(편집 후).
export function useDataSourceCatalog(): { sources: DataSource[]; loading: boolean; error: string; reload: () => void } {
  const [, setTick] = useState(0)
  useEffect(() => {
    const sub = () => setTick(t => t + 1)
    _subs.add(sub)
    if (!_started) { _started = true; void _fetch() }
    return () => { _subs.delete(sub) }
  }, [])
  return {
    sources: _catalog ?? [], loading: _loading, error: _error,
    reload: () => { void _fetch() },
  }
}

// co-placed 위젯이 같은 소스를 쓰면 load 중복 방지 — (sourceId|date|gran) 단기 캐시.
const _cache = new Map<string, { ts: number; p: Promise<unknown> }>()
const TTL_MS = 4000

export function loadSource(src: DataSource, params: SourceParams): Promise<unknown> {
  const key = `${src.id}|${params.date}|${params.granularity}`
  const hit = _cache.get(key)
  const now = Date.now()
  if (hit && now - hit.ts < TTL_MS) return hit.p
  const p = src.load(params)
  _cache.set(key, { ts: now, p })
  p.catch(() => { if (_cache.get(key)?.p === p) _cache.delete(key) })
  return p
}
