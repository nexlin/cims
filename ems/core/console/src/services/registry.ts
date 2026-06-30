// 활성 서비스 pack 레지스트리 — 콘솔에 얹을 서비스 매니페스트 목록.
//
// 새 서비스를 붙이려면: services/<svc>/manifest.tsx 작성 후 여기에 추가.
// (향후 빌드타임 env 또는 런타임 config 로 on/off 토글 가능하도록 확장 여지.)

import type { ServiceManifest } from '../nav-types'
import { cimsManifest } from '@svc/manifest'

// base 프로파일(부트스트랩 동봉 콘솔)은 서비스 pack 미포함 — 메뉴·위젯 모두 제외(DCE).
// 서비스 메뉴/위젯은 3·4단계에서 풀 프로파일 console 패키지 업데이트로 도착한다.
// (정확한 import.meta.env.X 구문 — vite 가 빌드 시 리터럴 치환 → 상수 조건 → DCE)
const IS_BASE_PROFILE = import.meta.env.VITE_CONSOLE_PROFILE === 'base'

export const SERVICE_MANIFESTS: ServiceManifest[] = IS_BASE_PROFILE ? [] : [
  cimsManifest,
]
