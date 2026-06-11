// 범용 OAM 콘솔 nav 타입 — routes.tsx 와 services/*/manifest.ts 가 공유 (순환 import 방지용 분리).
//
// 아키텍처: 콘솔 = [범용 OAM 코어 섹션] + [서비스 pack 이 기여하는 섹션] 의 합성.
//   - 코어 섹션 (CORE_SECTIONS): 대시보드 / 패키징 / 배포 / 문서 — 서비스 무지.
//   - 서비스 섹션 (ServiceManifest.sections): 가입자/서비스/통계 등 — CIMS 같은 서비스 pack 이 등록.
// routes.tsx 가 둘을 order 기준 병합해 SECTIONS 를 만든다.

import type { ComponentType } from 'react'
import type { LucideIcon } from 'lucide-react'
import type { WidgetDef, PageLayout } from './widgets/types'
import type { Role } from './api/auth'

export type RouteDef = {
  path: string
  title: string
  // 고정 페이지 컴포넌트. App 이 단일 page 위젯으로 감싸 편집 가능하게 렌더한다.
  // layout 이 지정된 합성 라우트는 component 생략 가능.
  component?: ComponentType
  // 합성 레이아웃 seed (대시보드/출력 페이지). 지정 시 component 대신 이 레이아웃을 EditableLayout 으로 렌더.
  layout?: PageLayout
  // 레이아웃 영속 키. 미지정 시 component 페이지는 'page:<path>', 합성 라우트는 layout.id/path.
  layoutId?: string
  // 접근 최소 역할 등급 (RBAC). 미지정 시 adminOnly=true→'admin', 아니면 'monitor'.
  requiredRole?: Role
  adminOnly?: boolean
  // 사이드바 하위항목에서 숨김 — 라우트 자체는 활성 (link/직접 URL 진입 가능).
  hidden?: boolean
  // 개발자 모드(admin 토글) ON 일 때만 노출 — 릴리스(빌드/검증/패키징) 등 공급사 개발 기능
  devOnly?: boolean
}

// OAM 대영역 — EMS(Nokia NetAct 의 Monitor/Administer, TM Forum eTOM 의 Assurance/Fulfillment) 관례.
//   ops(운용)   = 모니터링/실시간 = 대시보드·장애·성능·기록 (FCAPS 의 F/P + Accounting)
//   admin(관리) = 프로비저닝/구성/유지보수 = 구성·시스템·릴리스·문서 (FCAPS 의 C + SW Mgmt)
export type NavArea = 'ops' | 'admin'
export const NAV_AREA_ORDER: NavArea[] = ['ops', 'admin']
export const NAV_AREA_LABELS: Record<NavArea, string> = { ops: '운용', admin: '관리' }

export type RouteSection = {
  key: string
  label: string
  icon: LucideIcon
  basePath: string
  defaultPath: string
  routes: RouteDef[]
  // OAM 대영역 (운용/관리). 미지정 시 'admin' 취급.
  area?: NavArea
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
  widgets?: WidgetDef[]      // registry 가 수집해 layout(PageLayout)에서 배치
  // 데이터 소스는 manifest 가 아니라 Service Descriptor(data_sources, 백엔드 데이터)로 등록 —
  // shape 위젯이 /service-descriptors/data-sources 카탈로그를 소비 (완전 데이터 구동).
}
