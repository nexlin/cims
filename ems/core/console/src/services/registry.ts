// 활성 서비스 pack 레지스트리 — 콘솔에 얹을 서비스 매니페스트 목록.
//
// 새 서비스를 붙이려면: ems/<pack>/console/src/manifest.tsx 작성 + vite/tsconfig alias + 여기에 추가.
// (향후 빌드타임 env 또는 런타임 config 로 on/off 토글 가능하도록 확장 여지.)

import type { ServiceManifest } from '../nav-types'
import { cimsManifest } from '@svc/manifest'
import { testerManifest } from '@tester/manifest'

// 번들은 하나다 — 팩 전부를 담아 `oam`(base) 패키지에 동봉한다(oam_base_service_split D1).
// 어느 팩의 메뉴가 보이는지는 빌드 프로파일이 아니라 **설치된 서비스**로 정한다: 섹션의
// `requiresService`(패키지 id) 를 셸(MenuContext)이 `GET /console/catalog`.installed_services 로
// 게이팅한다. 부트스트랩 직후(base 만)는 코어 섹션만 보이고, 서비스 모듈이 설치되면 재로그인
// 없이 다음 조회에서 그 팩의 메뉴가 나타난다.
export const SERVICE_MANIFESTS: ServiceManifest[] = [
  cimsManifest,     // CIMS 서비스 팩(ems/service/console) — requiresService='oam-svc'
  testerManifest,   // 계측기 팩(ems/tester/console)     — requiresService='oam-cims-tester'
]
