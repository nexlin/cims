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

export const api = {
  get:    <T>(path: string)                   => request<T>('GET',    path),
  post:   <T>(path: string, body: unknown)    => request<T>('POST',   path, body),
  put:    <T>(path: string, body: unknown)    => request<T>('PUT',    path, body),
  delete: <T>(path: string, body?: unknown)    => request<T>('DELETE', path, body),
}
