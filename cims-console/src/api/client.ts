const BASE = '/api/v1'

// 상대경로(Vite dev proxy / nginx proxy) 또는 직접 모드 (VITE_CSC_DIRECT=1) 자동 분기.
// 직접 모드는 console 정적 서빙 호스트와 다른 포트에 있는 CSC 로 CORS 호출 (CSC 가 CORS 허용).
function buildApiUrl(path: string): string {
  const env = (import.meta as unknown as { env: Record<string, string> }).env || {}
  if (env.VITE_CSC_DIRECT === '1' && env.PROD !== 'true') {
    const loc = window.location
    const port = env.VITE_CSC_PORT || '4420'
    return `${loc.protocol}//${loc.hostname}:${port}${BASE}${path}`
  }
  return `${BASE}${path}`
}

export class ApiError extends Error {
  status: number
  data: Record<string, unknown>
  constructor(message: string, status: number, data: Record<string, unknown>) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.data = data
  }
}

function getToken(): string | null {
  return localStorage.getItem('cims_token')
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { 'Content-Type': 'application/json' }
  const token = getToken()
  if (token) headers['Authorization'] = `Bearer ${token}`

  const res = await fetch(buildApiUrl(path), {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  })
  const data = await res.json().catch(() => ({}))
  if (!res.ok) {
    if (res.status === 401) {
      localStorage.removeItem('cims_token')
      window.location.reload()
    }
    const obj = (data ?? {}) as Record<string, unknown>
    const msg = (typeof obj.error === 'string' && obj.error) || `HTTP ${res.status}`
    throw new ApiError(msg, res.status, obj)
  }
  return data as T
}

// ── 클라이언트 캐시 + in-flight 중복제거 ────────────────────────────────
// 목적: 이미 받아온 데이터를 재사용해 백엔드 재조회·재연산을 최소화(사용자 요구).
//  - in-flight 중복제거: 동시에 뜬 동일 GET 은 1개의 네트워크 요청만(리렌더로 인한 중복 호출 흡수).
//  - getCached: 짧은 TTL 동안 응답 재사용(페이지 재진입·아코디언 재오픈 시 즉시 표시, 백엔드 무부하).
//  - 변이(POST/PUT/DELETE) 시 캐시 무효화 → 목록/상세가 stale 되지 않음.
// (react-query/SWR 의 핵심만 의존성 없이 최소 구현 — 추후 MobX/스토어로 확장 가능한 토대.)
interface CacheEntry { ts: number; ttl: number; data: unknown }
const _cache = new Map<string, CacheEntry>()
const _inflight = new Map<string, Promise<unknown>>()

function _getDedup<T>(path: string): Promise<T> {
  const key = 'GET ' + path
  const existing = _inflight.get(key)
  if (existing) return existing as Promise<T>
  const p = request<T>('GET', path).finally(() => { _inflight.delete(key) })
  _inflight.set(key, p as Promise<unknown>)
  return p
}

export const apiCache = {
  /** 경로 substring 매칭으로 캐시 무효화 (인자 없으면 전체) */
  invalidate(match?: string) {
    if (!match) { _cache.clear(); return }
    for (const k of _cache.keys()) if (k.includes(match)) _cache.delete(k)
  },
}

export const api = {
  get:    <T>(path: string)                   => _getDedup<T>(path),
  /** TTL(ms) 동안 응답 재사용 + in-flight 중복제거. 읽기전용 무거운 목록/플로우에 사용. */
  getCached<T>(path: string, ttlMs = 4000): Promise<T> {
    const hit = _cache.get(path)
    const now = Date.now()
    if (hit && (now - hit.ts) < hit.ttl) return Promise.resolve(hit.data as T)
    return _getDedup<T>(path).then(data => {
      _cache.set(path, { ts: now, ttl: ttlMs, data })
      return data
    })
  },
  post:   <T>(path: string, body: unknown)    => { apiCache.invalidate(); return request<T>('POST',   path, body) },
  put:    <T>(path: string, body: unknown)    => { apiCache.invalidate(); return request<T>('PUT',    path, body) },
  delete: <T>(path: string, body?: unknown)    => { apiCache.invalidate(); return request<T>('DELETE', path, body) },
}
