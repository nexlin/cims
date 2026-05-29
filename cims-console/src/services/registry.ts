// 활성 서비스 pack 레지스트리 — 콘솔에 얹을 서비스 매니페스트 목록.
//
// 새 서비스를 붙이려면: services/<svc>/manifest.tsx 작성 후 여기에 추가.
// (향후 빌드타임 env 또는 런타임 config 로 on/off 토글 가능하도록 확장 여지.)

import type { ServiceManifest } from '../nav-types'
import { cimsManifest } from './cims/manifest'

export const SERVICE_MANIFESTS: ServiceManifest[] = [
  cimsManifest,
]
