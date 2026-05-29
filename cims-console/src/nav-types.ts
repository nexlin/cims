// 범용 OAM 콘솔 nav 타입 — routes.tsx 와 services/*/manifest.ts 가 공유 (순환 import 방지용 분리).
//
// 아키텍처: 콘솔 = [범용 OAM 코어 섹션] + [서비스 pack 이 기여하는 섹션] 의 합성.
//   - 코어 섹션 (CORE_SECTIONS): 대시보드 / 패키징 / 배포 / 문서 — 서비스 무지.
//   - 서비스 섹션 (ServiceManifest.sections): 가입자/서비스/통계 등 — CIMS 같은 서비스 pack 이 등록.
// routes.tsx 가 둘을 order 기준 병합해 SECTIONS 를 만든다.

import type { ComponentType } from 'react'
import type { LucideIcon } from 'lucide-react'
import type { WidgetDef } from './widgets/types'

export type RouteDef = {
  path: string
  title: string
  component: ComponentType
  adminOnly?: boolean
  // SubTabs 에서 숨김 — 라우트 자체는 활성 (link 으로 진입 가능).
  hidden?: boolean
}

export type RouteSection = {
  key: string
  label: string
  icon: LucideIcon
  basePath: string
  defaultPath: string
  routes: RouteDef[]
  // nav 정렬 순서 (작을수록 앞). 미지정 시 50. 코어/서비스 섹션 병합 시 사용.
  order?: number
  // 이 섹션을 기여한 서비스 pack id (코어 섹션은 undefined). 향후 service on/off 토글용.
  serviceId?: string
  // VITE_CONSOLE_TARGET=prod 빌드에서 숨김 (배포 콘솔 = 운영자용).
  prodHidden?: boolean
}

// 서비스 pack 이 콘솔에 기여하는 내용 — nav 섹션 + 위젯(대시보드/페이지 합성용).
export interface ServiceManifest {
  id: string         // 'cims'
  label: string      // 'CIMS'
  sections: RouteSection[]
  widgets?: WidgetDef[]   // registry 가 수집해 layout(PageLayout)에서 배치
}
