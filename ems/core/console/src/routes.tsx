// 콘솔 라우트 = [범용 OAM 코어 섹션] + [서비스 pack 기여 섹션] 합성.
//   - CORE_SECTIONS: 대시보드 / 장애 / 시스템 / 릴리스 / 문서 — 서비스 무지(범용 OAM).
//   - SERVICE_MANIFESTS: 구성(가입자)/성능/기록 등 — services/registry.ts 의 서비스 pack 이 기여.
// 메뉴는 OAM 표준(FCAPS) + EMS(NetAct/ENM/U2000) 관례에 맞춰 운용(ops)/관리(admin) 2대영역 그룹.
// nav 타입은 ./nav-types, CIMS 기여는 ./services/cims/manifest.
import {
  LayoutDashboard,
  Bell,
  Server,
  Rocket,
  BookOpen,
} from 'lucide-react'

import type { RouteDef, RouteSection } from './nav-types'
import { SERVICE_MANIFESTS } from './services/registry'

import { DASHBOARD_LAYOUT } from './widgets/layouts'
import AlertsPage from './pages/AlertsPage'
import ServicesPage from './pages/ServicesPage'
import PackagesPage from './pages/PackagesPage'
import ServersPage from './pages/ServersPage'
import HaServicesPage from './pages/HaServicesPage'
import VerificationV2Page from './pages/VerificationV2Page'
import VerificationHistoryPage from './pages/VerificationHistoryPage'
import DocsPage from './pages/DocsPage'
import ExternalSystemsPage from './pages/ExternalSystemsPage'
import ConsoleAccountsPage from './pages/ConsoleAccountsPage'
import MyLayoutPage from './pages/MyLayoutPage'

export type { RouteDef, RouteSection } from './nav-types'

// 콘솔 타겟 — 'dev' (TB-Console / 검증용) | 'prod' (배포본 — 운영자용, 제한된 메뉴)
const CONSOLE_TARGET = ((import.meta as unknown as { env: Record<string, string> }).env?.VITE_CONSOLE_TARGET) || 'dev'
export const IS_PROD_CONSOLE = CONSOLE_TARGET === 'prod'

// 콘솔 프로파일 — 'full'(기본) | 'base'(부트스트랩 동봉본: 관리>시스템 + 관리>릴리스(개발자모드)만).
// base 는 서비스 무관 코어만 담는다 — 서비스 메뉴/위젯은 3·4단계(패키지 등록/설치)에서
// 풀 프로파일 console 패키지로 업데이트되며 도착한다 (services/registry.ts 도 동일 게이트).
// (정확한 import.meta.env.X 구문 — vite 빌드 시 리터럴 치환 → 상수 조건 → 서비스 코드 DCE)
export const IS_BASE_CONSOLE = import.meta.env.VITE_CONSOLE_PROFILE === 'base'
const BASE_PROFILE_SECTION_KEYS = new Set(['system', 'release'])

// 활성 알람 뷰 (해소된 알람 숨김). 이력 뷰는 AlertsPage 기본.
const ActiveAlarmsPage = () => <AlertsPage openOnly />

// ── 범용 OAM 코어 섹션 (서비스 무지) ──────────────────────────────
const CORE_SECTIONS: RouteSection[] = [
  // ── 운용(ops) ──
  {
    key: 'dashboard',
    label: '대시보드',
    icon: LayoutDashboard,
    area: 'ops',
    basePath: '/dashboard',
    defaultPath: '/dashboard',
    order: 10,
    routes: [
      { path: '/dashboard', title: '종합 현황', layout: DASHBOARD_LAYOUT, layoutId: 'dashboard' },
      // 콘솔 D1 — 계정별 개인 대시보드 구성(프로파일+위젯). 서버 저장(/console/layouts/me).
      { path: '/dashboard/my-layout', title: '내 대시보드 구성', component: MyLayoutPage },
    ],
  },
  {
    key: 'fault',
    label: '장애',
    icon: Bell,
    area: 'ops',
    basePath: '/alerts',
    defaultPath: '/alerts/active',
    order: 20,
    routes: [
      { path: '/alerts/active',  title: '활성 알람',        component: ActiveAlarmsPage },
      { path: '/alerts/history', title: '알람·이벤트 이력', component: AlertsPage },
    ],
  },
  // ── 관리(admin) ──
  {
    key: 'system',
    label: '시스템',
    icon: Server,
    area: 'admin',
    basePath: '/deploy',
    defaultPath: '/deploy/servers',
    order: 60,
    routes: [
      { path: '/deploy/servers',          title: '시스템/인프라', component: ServersPage,         requiredRole: 'operator' },
      // ↑ RBAC: 페이지 진입 operator+. 탭1(시스템/서버 구성)·탭2(패키지 설치)·탭4(패키지 제어)는
      //   조회만 가능, 변이는 admin 세션 또는 admin 패스워드 승격. 탭3(패키지 설정)=operator 편집.
      { path: '/deploy/external-systems', title: '외부 시스템',   component: ExternalSystemsPage, adminOnly: true },
      { path: '/deploy/console-accounts', title: '콘솔 계정',     component: ConsoleAccountsPage, adminOnly: true },
      { path: '/deploy/packages',         title: '패키지',        component: PackagesPage,        adminOnly: true },
      // HA 상세 편집 — ServersPage(시스템/인프라) 인스펙터가 HA 를 인라인 편집하므로
      // 사이드바 leaf 에서는 숨김(중복 제거). route 는 ServersPage 의 deep-link(?group=)용 유지.
      { path: '/deploy/services',  title: 'HA 상세 편집',  component: HaServicesPage,  adminOnly: true, hidden: true },
    ],
  },
  {
    key: 'release',
    label: '릴리스',
    icon: Rocket,
    area: 'admin',
    basePath: '/release',
    defaultPath: '/release/verify',
    order: 70,
    // prodHidden 제거(2026-06-11): 빌드타임 숨김 대신 런타임 '개발자 모드'(devOnly)가
    // 게이팅 — 상용 base 콘솔에서도 admin 이 개발자 모드를 켜면 릴리스 메뉴 사용 가능.
    routes: [
      { path: '/release/verify',          title: '검증',         component: VerificationV2Page, adminOnly: true, devOnly: true },
      { path: '/release/verify-history',  title: '검증 이력',     component: VerificationHistoryPage, adminOnly: true, devOnly: true },
      { path: '/release/package',         title: '패키징',       component: ServicesPage, adminOnly: true, devOnly: true },
    ],
  },
  {
    key: 'docs',
    label: '문서',
    icon: BookOpen,
    area: 'admin',
    basePath: '/docs',
    defaultPath: '/docs',
    order: 90,
    routes: [
      { path: '/docs', title: '문서', component: DocsPage },
    ],
  },
]

// 코어 + 서비스 pack 섹션 병합 → order 기준 정렬. 서비스 섹션엔 serviceId 태깅.
const SERVICE_SECTIONS: RouteSection[] = SERVICE_MANIFESTS.flatMap(
  m => m.sections.map(s => ({ ...s, serviceId: s.serviceId ?? m.id }))
)

export const SECTIONS: RouteSection[] = [...CORE_SECTIONS, ...SERVICE_SECTIONS]
  .filter(s => !IS_BASE_CONSOLE || BASE_PROFILE_SECTION_KEYS.has(s.key))
  .sort((a, b) => (a.order ?? 50) - (b.order ?? 50))

// IS_PROD_CONSOLE 일 때 prodHidden=true 섹션 제거. dev 빌드는 모두 노출.
export const VISIBLE_SECTIONS: RouteSection[] = SECTIONS.filter(s => !IS_PROD_CONSOLE || !s.prodHidden)

// 평탄화된 라우트 리스트 — <Routes> 렌더용
export const FLAT_ROUTES: RouteDef[] = VISIBLE_SECTIONS.flatMap(s => s.routes)

// 로그인 직후/루트(/) 랜딩 경로 — base 프로파일엔 /dashboard 가 없으므로 첫 섹션 기본 경로.
export const HOME_PATH: string = VISIBLE_SECTIONS.find(s => s.key === 'dashboard')?.defaultPath
  ?? VISIBLE_SECTIONS[0]?.defaultPath ?? '/dashboard'

// 경로 → section/route 조회. 섹션이 자기 basePath 밖의 route 도 가질 수 있어(예: 구성↔서비스정의)
// basePath prefix 가 아니라 route 멤버십으로 매칭한다.
export function findSectionByPath(pathname: string): RouteSection | undefined {
  return SECTIONS.find(s => s.routes.some(r => pathname === r.path || pathname.startsWith(r.path + '/')))
}

export function findRouteByPath(pathname: string): RouteDef | undefined {
  return FLAT_ROUTES.find(r => r.path === pathname)
}
