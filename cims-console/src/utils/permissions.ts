// 콘솔 RBAC 권한 헬퍼 — docs/design/features/mcptt_authorization.md §3.
//   계층적 5종(admin>manager>operator>monitor>user). 백엔드(CSC/OAM)가 최종 게이트이고,
//   여기서는 메뉴/버튼 노출 결정만 한다.
import type { AuthUser, Role } from '../api/auth'

// developer = 공급사 개발 계정 (패키지 내장, DB 비저장) — 권한은 admin 동급.
const RANK: Record<Role, number> = { user: 0, monitor: 1, operator: 2, manager: 3, admin: 4, developer: 4 }

export const ROLE_LABELS: Record<Role, string> = {
  admin:    '관리자',
  developer: '개발자',
  manager:  '운영 관리자',
  operator: '운용자',
  monitor:  '모니터',
  user:     '일반 사용자',
}

// 콘솔 계정으로 지정 가능한 역할 (user 는 telephony 전용 → 콘솔 로그인 불가, 보통 미지정).
// developer 는 패키지 내장 전용 — DB 계정 role 로 지정 불가.
export const ASSIGNABLE_ROLES: Role[] = ['admin', 'manager', 'operator', 'monitor', 'user']

export function roleRank(role?: string | null): number {
  return RANK[(role as Role)] ?? 0
}

/** user 가 min 등급 이상인가 */
export function hasRole(user: AuthUser | null | undefined, min: Role): boolean {
  return roleRank(user?.role) >= RANK[min]
}

// ── 도메인별 능력 ──────────────────────────────────────────────
/** 구성(가입자/조직) 쓰기 — manager+ */
export const canWriteConfig = (u: AuthUser | null | undefined) => hasRole(u, 'manager')
/** 역할(권한) 지정 — admin 만 */
export const canAssignRole = (u: AuthUser | null | undefined) => hasRole(u, 'admin')
/** PTT 그룹 생성 — operator+ */
export const canCreateGroup = (u: AuthUser | null | undefined) => hasRole(u, 'operator')
/** 특정 그룹 편집/삭제 — manager+ 전체, operator 는 본인 소유만 */
export function canManageGroup(u: AuthUser | null | undefined, authorizedUserId?: number | null): boolean {
  if (hasRole(u, 'manager')) return true
  return hasRole(u, 'operator') && !!u && authorizedUserId != null && u.id === authorizedUserId
}

// ── 라우트(메뉴) 게이팅 ────────────────────────────────────────
import type { RouteDef } from '../nav-types'

/** 라우트 접근 최소 등급. requiredRole > adminOnly(=admin) > 기본 monitor. */
export function routeMinRole(route: RouteDef): Role {
  if (route.requiredRole) return route.requiredRole
  if (route.adminOnly) return 'admin'
  return 'monitor'
}

export function canAccessRoute(user: AuthUser | null | undefined, route: RouteDef): boolean {
  return hasRole(user, routeMinRole(route))
}
