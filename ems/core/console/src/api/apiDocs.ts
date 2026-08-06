// API 문서 (개발자 모드) — 각 모듈이 코드 옆에 선언한 자기기술을 OAM 이 수집해 준다.
// 콘솔은 저장·가공하지 않고 읽어서 표시만 한다. 모듈 미설치/미가용이면 응답에서 빠진다.
// 어떤 위젯이 어떤 API 를 쓰는지는 WidgetDef.apis(id 목록)가 선언한다 — 내용은 전부 여기서 온다.
import { api } from './client'

export interface ApiDocParam {
  name: string
  in: 'query' | 'path' | 'body' | string
  type?: string
  required?: boolean
  enum?: string[]
  desc?: string
}

export interface ApiDoc {
  id: string
  module: string | null      // 'csc' | 'oam-svc' | null(base 상주)
  method: string
  path: string               // /api/v1 포함 전체 경로
  summary?: string
  params?: ApiDocParam[]
  response?: string
  auth?: string
}

export interface ApiDocsResponse {
  modules: string[]
  count: number
  apis: ApiDoc[]
}

// 한 화면에 배지가 여러 개(위젯 수만큼) 뜨므로 요청을 공유한다 — 페이지당 1회.
let _inflight: Promise<Map<string, ApiDoc>> | null = null

export function loadApiDocs(): Promise<Map<string, ApiDoc>> {
  if (!_inflight) {
    _inflight = api.get<ApiDocsResponse>('/api-docs')
      .then(r => new Map((r.apis || []).map(a => [a.id, a])))
      .catch(() => new Map<string, ApiDoc>())
  }
  return _inflight
}

// 개발자 모드 토글/재로그인 등으로 다시 받아야 할 때.
export function resetApiDocs() { _inflight = null }
