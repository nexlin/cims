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

// 응답 필드 1개. name 은 `a.b[].c`(배열) / `a.b{}`(맵) 표기로 중첩을 표현한다.
export interface ApiDocField {
  name: string
  type?: string
  unit?: string
  enum?: string[]
  desc?: string
}

export interface ApiDocError {
  status: number
  when?: string
  body?: unknown
}

// 인증 — 구조화된 형태. 구 선언(문자열)도 렌더러가 그대로 표시한다.
export interface ApiDocAuth {
  scheme?: string
  role?: string
  token_from?: string
  note?: string
}

export interface ApiDoc {
  id: string
  module: string | null      // 'csc' | 'oam-svc' | null(base 상주)
  method: string
  path: string               // /api/v1 포함 전체 경로
  summary?: string
  params?: ApiDocParam[]
  response?: string          // 한 줄 요약 (구조는 response_fields)
  response_fields?: ApiDocField[]
  example?: unknown          // 합성 응답 예시 (실데이터 금지)
  errors?: ApiDocError[]
  notes?: string[]           // 기본값·제약·성능 주의
  auth?: ApiDocAuth | string
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
