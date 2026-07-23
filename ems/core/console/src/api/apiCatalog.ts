// 연동/API 카탈로그 client — OAM /api/v1/api-catalog.
// 외부(VoLTE/PTT 사용관리 웹 등)에 넘길 공유(READ) 엔드포인트 목록 + 생성된 OpenAPI 3 문서.
import { api } from './client'

export type ApiCategory = 'stats' | 'history' | 'recording' | 'subscriber' | string

export interface ApiParam {
  name: string
  in?: 'query' | 'path'
  type?: string
  required?: boolean
  enum?: string[]
  desc?: string
}

// descriptor shareable_apis[] 엔트리 (service_id 는 백엔드가 부착).
export interface ShareableApi {
  id: string
  method: string
  path: string
  summary: string
  category: ApiCategory
  params?: ApiParam[]
  auth?: string
  audience?: string
  response_desc?: string
  example?: unknown
  service_id?: string
}

export interface ApiCatalogResponse {
  generated_at: string
  count: number
  categories: string[]
  endpoints: ShareableApi[]
}

export const apiCatalogApi = {
  // 카탈로그 탭이 소비하는 목록.
  get: () => api.get<ApiCatalogResponse>('/api-catalog'),
  // 생성된 OpenAPI 3 문서(파싱된 JSON) — 복사/다운로드에 사용. 인증은 client.ts 가 처리.
  getOpenApi: () => api.get<Record<string, unknown>>('/api-catalog/openapi.json'),
}
