import { useState } from 'react'
import { BrowserRouter, Routes, Route, Navigate, useParams, useSearchParams } from 'react-router-dom'
import { AuthProvider, useAuth } from './contexts/AuthContext'
import { MenuProvider } from './contexts/MenuContext'
import { ToastProvider } from './components/Toast'
import Sidebar from './components/Sidebar'
import Header from './components/Header'
import ReadOnlyBanner from './components/ReadOnlyBanner'
import LoginPage from './pages/LoginPage'
import { FLAT_ROUTES, HOME_PATH } from './routes'
import { useMenu } from './contexts/MenuContext'
import { findCustomPage } from './menu'
import type { RouteDef } from './nav-types'
import { canAccessRoute } from './utils/permissions'
import { useDevMode } from './hooks/useDevMode'
import { setDevMode } from './utils/devMode'
import { authApi } from './api/auth'
import { EditableLayout } from './widgets/EditableLayout'
import { registerWidgets } from './widgets/registry'
import type { WidgetProps, PageLayout } from './widgets/types'
import { GRID_COLS, GRID_ROWS } from './widgets/gridLayout'
import type { ComponentType } from 'react'
import './index.css'

const SIDEBAR_COLLAPSED_KEY = 'cims_sidebar_collapsed'

// ── 모든 메뉴 페이지를 위젯 합성 surface 로 ──────────────────────────
// 고정 페이지(component)를 'page:<path>' 위젯으로 등록 → EditableLayout 으로 감싸 렌더하면
// 기본 모습은 동일하되 admin 이 위젯을 추가/배치할 수 있다. 합성 라우트(layout)는 그 seed 를 그대로.
const PAGE_WIDGET_PREFIX = 'page:'

// 고정 페이지 → page 위젯 정의. 모듈 로드 시 일괄 등록하고(아래), 렌더 시에도 보장 등록(idempotent)
// 한다 — 모듈 평가 순서/HMR 에 흔들리지 않도록.
//
// 본문은 `.page-scroll` 로 감싼다 — **페이지도 위젯과 같은 규율**을 따라야 하기 때문이다
// (console_platform §3.0): 배치한 칸을 채우고, 넘치면 브라우저가 아니라 **그 안에서** 스크롤한다.
// 관제 화면이라 페이지 스크롤은 곧 "화면 밖으로 밀려남"이라 허용하지 않는다.
function pageWidgetDefs() {
  return FLAT_ROUTES.filter(r => r.component && !r.layout).map(r => {
    const Page = r.component as unknown as ComponentType<WidgetProps>
    return {
      id: PAGE_WIDGET_PREFIX + r.path,
      title: r.title,
      category: 'page' as const,
      component: (props: WidgetProps) => <div className="page-scroll"><Page {...props} /></div>,
      adminOnly: r.adminOnly,
      apis: r.apis,          // 고정 페이지의 사용 API — 개발자 모드 [API] 배지용
    }
  })
}
registerWidgets(pageWidgetDefs())

// 레이아웃 영속 id 는 URL path 세그먼트로 쓰이므로 slash-free 여야 한다 — 백엔드가 경로의
// %2F 를 '/' 로 디코드해 'page:/alerts/active' 를 'page:' 까지만 키로 파싱(전 page 충돌)하기 때문.
function routeLayoutId(r: RouteDef): string {
  if (r.layoutId) return r.layoutId
  if (r.layout) return r.layout.id || r.path.replace(/^\/+/, '').replace(/\//g, '.')
  return PAGE_WIDGET_PREFIX + r.path.replace(/^\/+/, '').replace(/\//g, '.')
}
// 고정 페이지의 기본 배치 = **캔버스 한 장을 통째로 차지하는 위젯 하나**.
// 예전엔 legacy flow(`w: 12`, 높이 미지정)라 페이지가 내용만큼 자라 브라우저가 스크롤됐다.
// 이제 grid 좌표로 48×48 을 채우므로 어떤 페이지든 한 화면 안이고, 넘치는 내용은 `.page-scroll`
// 안에서 스크롤한다(§3.0). seedVersion 은 저장본이 옛 flow 배치를 붙들고 있을 때 안내를 띄우기 위함.
function routeSeed(r: RouteDef): PageLayout {
  if (r.layout) return r.layout
  return {
    id: routeLayoutId(r), seedVersion: 1,
    widgets: [{ widgetId: PAGE_WIDGET_PREFIX + r.path, x: 0, y: 0, w: GRID_COLS, h: GRID_ROWS }],
  }
}

// 모든 라우트를 EditableLayout 으로 렌더 — 합성 라우트는 자기 seed, 고정 페이지는 단일 page 위젯.
function EditablePageHost({ route }: { route: RouteDef }) {
  // 이 라우트의 page 위젯을 보장 등록 — 모듈 로드 시 일괄 등록이 어떤 이유로 누락돼도
  // (HMR/평가 순서) GridRenderer 가 위젯을 찾도록. registerWidgets 는 idempotent(중복 무시).
  if (route.component && !route.layout) {
    registerWidgets([{
      id: PAGE_WIDGET_PREFIX + route.path,
      title: route.title,
      category: 'page' as const,
      component: route.component as unknown as ComponentType<WidgetProps>,
      adminOnly: route.adminOnly,
      apis: route.apis,
    }])
  }
  // key=layoutId — 모든 라우트가 동일한 EditableLayout 타입이라, 메뉴 전환 시 React 가 인스턴스를
  // 재사용(업데이트)해 EditableLayout 의 useState(seed) 가 이전 페이지 그대로 남는다(전환 안 됨).
  // layoutId 별 key 로 강제 리마운트 → 새 seed/layout 로 초기화.
  const layoutId = routeLayoutId(route)
  return <EditableLayout key={layoutId} layoutId={layoutId} seed={routeSeed(route)} />
}

// 메뉴 편집으로 추가한 커스텀 페이지(/custom/<slug>) — 빈 위젯 seed 의 EditableLayout.
// admin 이 위젯을 배치해 페이지를 구성한다. 메뉴(저장본)에서 제거된 slug 도 URL 직접 접근은
// 허용 (저장된 layout 이 남아 있으면 그대로 렌더 — 링크 공유/복구 여지).
function CustomPageHost() {
  const { slug = '' } = useParams()
  const { savedConfig } = useMenu()
  const found = findCustomPage(savedConfig, slug)
  const title = found?.page.title ?? slug
  const layoutId = `custom.${slug}`
  return <EditableLayout key={layoutId} layoutId={layoutId}
    seed={{ id: layoutId, title, widgets: [] }} />
}

// 옛 HA 상세 편집(/deploy/services?group=N) → 시스템/인프라의 같은 그룹 인스펙터.
// 쿼리를 보존해야 북마크가 원래 보던 그룹으로 그대로 떨어진다.
function HaServicesRedirect() {
  const [params] = useSearchParams()
  const gid = params.get('group')
  return <Navigate to={gid ? `/deploy/servers?group=${gid}` : '/deploy/servers'} replace />
}

function RouteGuard({ children, route }: { children: React.ReactNode; route: RouteDef }) {
  const { user } = useAuth()
  const devMode = useDevMode()
  if (!canAccessRoute(user, route)) {
    return <div className="empty" style={{ marginTop: 80 }}>접근 권한이 없습니다</div>
  }
  // 개발 기능(릴리스 등)은 개발자 모드에서만 — 권한이 아닌 화면 모드 분리
  if (route.devOnly && !devMode) {
    return (
      <div className="empty" style={{ marginTop: 80, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 12 }}>
        <div>이 메뉴는 <b>개발자 모드</b>에서 사용합니다 (빌드·검증·패키징).</div>
        <button className="btn btn--primary btn--sm" onClick={() => setDevMode(true)}>
          {'</>'} 개발자 모드 켜기
        </button>
      </div>
    )
  }
  return <>{children}</>
}

function Shell() {
  const { user, loading, logout, refresh } = useAuth()
  const [collapsed, setCollapsed] = useState<boolean>(() => {
    return localStorage.getItem(SIDEBAR_COLLAPSED_KEY) === '1'
  })
  const [showChgPw, setShowChgPw] = useState(false)
  const [chgError, setChgError] = useState('')
  const [chgOk, setChgOk] = useState('')
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [newPw2, setNewPw2] = useState('')

  if (loading) return <div className="auth-loading">로딩 중...</div>
  if (!user) return <LoginPage />

  function toggleSidebar() {
    setCollapsed(prev => {
      const next = !prev
      localStorage.setItem(SIDEBAR_COLLAPSED_KEY, next ? '1' : '0')
      return next
    })
  }

  async function handleChangePassword(e: React.FormEvent) {
    e.preventDefault()
    setChgError(''); setChgOk('')
    if (newPw !== newPw2) { setChgError('새 비밀번호가 일치하지 않습니다'); return }
    if (newPw.length < 4) { setChgError('4자 이상이어야 합니다'); return }
    try {
      await authApi.changePassword(oldPw, newPw)
      await refresh()
      setChgOk('비밀번호가 변경되었습니다')
      setOldPw(''); setNewPw(''); setNewPw2('')
      setTimeout(() => { setShowChgPw(false); setChgOk('') }, 1500)
    } catch (err: unknown) {
      setChgError((err as Error).message)
    }
  }

  return (
    <ToastProvider>
     <MenuProvider>
      <div className={`app-layout ${collapsed ? 'app-layout--collapsed' : ''}`}>
        <Header
          userName={user.name}
          userRole={user.role}
          onLogout={logout}
          onChangePw={() => setShowChgPw(true)}
        />
        <Sidebar collapsed={collapsed} onToggle={toggleSidebar} />
        <main className="app-content">
          {/* 관리 store 소유권 미보유(read-only) 경고 — 정상 상태에서는 렌더 안 함 */}
          <ReadOnlyBanner />
          <div className="app-content-body">
            <Routes>
              <Route path="/" element={<Navigate to={HOME_PATH} replace />} />
              {/* 옛 경로 호환 — 알람 이력은 /alerts/history 로 이전 */}
              <Route path="/dashboard/alerts" element={<Navigate to="/alerts/history" replace />} />
              {/* 옛 경로 호환 — 자동 배포는 시스템 → 릴리스 그룹으로 이전 */}
              <Route path="/deploy/auto-deploy" element={<Navigate to="/release/auto-deploy" replace />} />
              {/* 옛 경로 호환 — SIP/CMP/CSC/HTTPS 별 메뉴와 서비스축 메시지 통계는
                  '인터페이스 통계' 한 화면으로 합쳐졌다(대상·서비스는 조회 조건). */}
              {['/stats/sip', '/stats/cmp', '/stats/csc', '/stats/https', '/stats/messages'].map(p => (
                <Route key={p} path={p} element={<Navigate to="/stats/interfaces" replace />} />
              ))}
              {/* 옛 경로 호환 — HA 상세 편집은 시스템/인프라 그룹 인스펙터로 흡수됨 */}
              <Route path="/deploy/services" element={<HaServicesRedirect />} />
              {FLAT_ROUTES.map(r => (
                <Route
                  key={r.path}
                  path={r.path}
                  element={<RouteGuard route={r}><EditablePageHost route={r} /></RouteGuard>}
                />
              ))}
              {/* 메뉴 편집으로 추가한 커스텀 위젯 페이지 */}
              <Route path="/custom/:slug" element={<CustomPageHost />} />
              <Route path="*" element={<Navigate to={HOME_PATH} replace />} />
            </Routes>
          </div>
        </main>
      </div>
     </MenuProvider>

      {showChgPw && (
        <div className="modal-overlay" onClick={() => setShowChgPw(false)}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">🔑 비밀번호 변경</span>
              <button className="modal-close" onClick={() => setShowChgPw(false)}>✕</button>
            </div>
            <form onSubmit={handleChangePassword}>
              <div className="modal-body">
                <div className="form-grid">
                  <label>현재 비밀번호</label>
                  <input className="form-input" type="password" value={oldPw} onChange={e => setOldPw(e.target.value)} />
                  <label>새 비밀번호</label>
                  <input className="form-input" type="password" value={newPw} onChange={e => setNewPw(e.target.value)} />
                  <label>새 비밀번호 확인</label>
                  <input className="form-input" type="password" value={newPw2} onChange={e => setNewPw2(e.target.value)} />
                </div>
                {chgError && <div className="auth-error" style={{ marginTop: 12 }}>{chgError}</div>}
                {chgOk && <div className="auth-ok" style={{ marginTop: 12 }}>{chgOk}</div>}
              </div>
              <div className="modal-footer">
                <button className="btn btn--outline" type="button" onClick={() => setShowChgPw(false)}>취소</button>
                <button className="btn btn--primary" type="submit">변경</button>
              </div>
            </form>
          </div>
        </div>
      )}
    </ToastProvider>
  )
}

export default function App() {
  return (
    <BrowserRouter>
      <AuthProvider>
        <Shell />
      </AuthProvider>
    </BrowserRouter>
  )
}
