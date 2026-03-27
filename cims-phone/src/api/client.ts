const BASE = '/api/v1'

function getToken(): string | null {
  return localStorage.getItem('cims_token')
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { 'Content-Type': 'application/json' }
  const token = getToken()
  if (token) headers['Authorization'] = `Bearer ${token}`

  const res = await fetch(`${BASE}${path}`, {
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
    throw new Error((data as { error?: string }).error ?? `HTTP ${res.status}`)
  }
  return data as T
}

export const api = {
  get:    <T>(path: string)                => request<T>('GET',  path),
  post:   <T>(path: string, body: unknown) => request<T>('POST', path, body),
}
