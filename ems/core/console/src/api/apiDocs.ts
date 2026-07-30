// API 문서 (개발자 모드) — 각 모듈이 코드 옆에 선언한 자기기술을 OAM 이 수집해 준다.
// 콘솔은 저장·가공하지 않고 읽어서 그대로 표시만 한다. 모듈 미설치/미가용이면 응답에서 빠진다.
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
  screens?: string[]         // 이 API 를 쓰는 콘솔 메뉴 경로
  summary?: string
  params?: ApiDocParam[]
  response?: string
  auth?: string
}

export interface ApiDocsResponse {
  screen: string | null
  modules: string[]
  count: number
  apis: ApiDoc[]
}

export const apiDocsApi = {
  // screen 지정 시 그 메뉴가 쓰는 API 만 (미지정 시 가용 모듈 전체)
  get: (screen?: string) =>
    api.get<ApiDocsResponse>(`/api-docs${screen ? `?screen=${encodeURIComponent(screen)}` : ''}`),
}
