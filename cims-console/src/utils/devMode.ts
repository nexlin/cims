// 개발자 모드 — admin 로그인 후 토글 (별도 developer 계정 없음, 2026-06-11 정책).
// ON 일 때만 개발(릴리스: 빌드/검증/패키징/배포검증) 메뉴·페이지 노출.
// 권한 분리가 아닌 화면 모드 분리 — 백엔드 권한은 admin rank 그대로.
// localStorage 영속 (브라우저 단위).

const KEY = 'cims_dev_mode'
const listeners = new Set<() => void>()

export function devModeActive(): boolean {
  try { return localStorage.getItem(KEY) === '1' } catch { return false }
}

export function setDevMode(on: boolean) {
  try {
    if (on) localStorage.setItem(KEY, '1')
    else localStorage.removeItem(KEY)
  } catch { /* noop */ }
  listeners.forEach(f => { try { f() } catch { /* noop */ } })
}

export function onDevModeChange(f: () => void): () => void {
  listeners.add(f)
  return () => { listeners.delete(f) }
}
